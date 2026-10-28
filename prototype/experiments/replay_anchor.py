"""离线回放原生扫描保存的页面，检查锚点±1px容差；不构建或运行C++，不操作游戏。"""

import argparse
import json
from pathlib import Path

import cv2
import numpy as np
from PIL import Image

from native_resolution import detect_bottom, samples


def load_page(path, layout, phase, scrolled):
    image = Image.open(path).convert('RGB')
    if list(image.size) != layout['reference_size']:
        raise ValueError(f'页面尺寸与扫描记录不符：{path}')
    bottom = detect_bottom(image, layout['bottom_detection'])
    cards = list(samples(image, layout, phase, scrolled, bottom))
    rows = sorted(set(y for _, _, _, y in cards))
    return {'gray': cv2.cvtColor(np.asarray(image), cv2.COLOR_RGB2GRAY), 'rows': rows,
            'cards': cards, 'phase': phase % layout['row_step'], 'bottom': bottom}


def raw_response(previous, current, layout, overlap):
    row_count = min(3, overlap)
    if len(previous['rows']) < overlap or overlap < 2:
        raise ValueError('上一屏完整行不足以选择锚点')
    start = len(previous['rows']) - overlap
    anchor_y = previous['rows'][start]
    x1, y1, x2, y2 = layout['name_tracking_rect']
    top = layout['scrolled_safe_top']
    bottom = layout['bottom_detection']['safe_bottom'] if current['bottom']['at_bottom'] else layout['safe_y'][1]
    last = bottom - layout['card_size'][1] - (row_count - 1) * layout['row_step']
    if last < top:
        raise ValueError('新画面安全区域不足以搜索锚点')
    responses = []
    for row in range(row_count):
        old_y = previous['rows'][start + row]
        for x in layout['columns']:
            template = previous['gray'][old_y + y1:old_y + y2, x + x1:x + x2]
            if template.std() < 3:
                raise ValueError('锚点名称尚未就绪')
            target = current['gray'][:, x + x1:x + x2]
            response = cv2.matchTemplate(target, template, cv2.TM_CCOEFF_NORMED).ravel()
            offset = y1 + row * layout['row_step']
            responses.append(response[top + offset:last + offset + 1])
    return anchor_y, top, np.stack(responses)


def align_response(raw):
    aligned = raw.copy()
    aligned[:, 1:] = np.maximum(aligned[:, 1:], raw[:, :-1])
    aligned[:, :-1] = np.maximum(aligned[:, :-1], raw[:, 1:])
    # 只比较搜索范围内的相邻响应，不跨边界环绕。
    if raw.shape[1] > 1:
        assert np.array_equal(aligned[:, 0], np.maximum(raw[:, 0], raw[:, 1]))
        assert np.array_equal(aligned[:, -1], np.maximum(raw[:, -1], raw[:, -2]))
    return aligned


def quality(scores, best, step):
    average = cv2.reduce(scores, 0, cv2.REDUCE_AVG).ravel()
    other = average[np.abs(np.arange(len(average)) - best) > step // 2]
    score, weakest = float(average[best]), float(scores[:, best].min())
    runner = float(other.max()) if len(other) else -1.
    return {'valid': score >= .9 and weakest >= .8 and score - runner >= .05,
            'score': score, 'weakest_score': weakest, 'runner_up': runner}


def locate(previous, current, layout, overlap):
    anchor_y, top, raw = raw_response(previous, current, layout, overlap)
    raw_average = cv2.reduce(raw, 0, cv2.REDUCE_AVG).ravel()
    best = int(raw_average.argmax())
    aligned = align_response(raw)
    before, after = quality(raw, best, layout['row_step']), quality(aligned, best, layout['row_step'])
    y = top + best
    after.update(y=y, displacement=anchor_y - y, raw_score=before['score'],
                 raw_weakest_score=before['weakest_score'], samples=[])
    for row, response in enumerate(raw):
        local = best
        for offset in (-1, 1):
            index = best + offset
            if 0 <= index < len(response) and response[index] > response[local]:
                local = index
        after['samples'].append({'row': row // len(layout['columns']) + 1,
                                 'column': row % len(layout['columns']) + 1,
                                 'vertical_offset': local - best,
                                 'raw_score': float(response[best]), 'score': float(response[local])})
    return {'anchor_y': anchor_y, 'y': y, 'displacement': anchor_y - y, 'before': before, 'after': after}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session', type=Path, help='包含frames及unmatched_frame.png的结果目录')
    parser.add_argument('--overlap-rows', type=int, default=6, help='原扫描使用的重叠行数')
    parser.add_argument('--output', type=Path, required=True, help='新建离线证据目录')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    document = json.loads((args.session / 'items.json').read_text('utf-8'))
    layout, items = document['layout'], document['items']
    transactions = [json.loads(line) for line in (args.session / 'transactions.jsonl').read_text('utf-8').splitlines()]
    frame_ids = list(dict.fromkeys(item['frame_id'] for item in items))
    records = []
    previous = None
    for index, frame_id in enumerate(frame_ids):
        first_item = next(item for item in items if item['frame_id'] == frame_id)
        page = load_page(args.session / 'frames' / f'{frame_id}.png', layout,
                         first_item['box'][1] % layout['row_step'], index > 0)
        if previous is not None:
            record = locate(previous, page, layout, args.overlap_rows)
            record.update(previous=frame_ids[index - 1], current=frame_id)
            logged = transactions[index]['trace'][-1]['anchor']
            assert record['y'] == logged['y'] and record['displacement'] == logged['displacement']
            assert abs(record['before']['score'] - logged['score']) < 1e-4
            assert abs(record['before']['weakest_score'] - logged['weakest_score']) < 1e-4
            assert record['before']['valid'] and record['after']['valid']
            assert record['displacement'] == transactions[index]['actual_pixels']
            records.append(record)
        previous = page

    unchanged = locate(previous, previous, layout, args.overlap_rows)
    assert unchanged['after']['valid'] and unchanged['displacement'] == 0

    failed_log = transactions[-1]['trace'][-1]['anchor']
    displacement = failed_log['displacement']
    last = load_page(args.session / 'unmatched_frame.png', layout,
                     previous['phase'] - displacement, True)
    failure = locate(previous, last, layout, args.overlap_rows)
    failure.update(previous=frame_ids[-1], current='unmatched_frame')
    assert failure['y'] == failed_log['y'] and failure['displacement'] == displacement
    assert abs(failure['before']['weakest_score'] - failed_log['weakest_score']) < 1e-4
    assert not failure['before']['valid'] and failure['after']['valid']
    records.append(failure)

    # 回放现有行号接续计算，只核对新增范围，不改写原扫描的部分结果。
    first_item = next(item for item in items if item['frame_id'] == frame_ids[-1])
    previous_start = first_item['row'] - 1 - (first_item['box'][1] - previous['rows'][0]) // layout['row_step']
    value = (last['rows'][0] - previous['rows'][0] + displacement) / layout['row_step']
    advance = int(np.floor(value + .5))
    assert advance >= 0 and abs(value - advance) <= .02
    current_start = previous_start + advance
    accepted_rows = max(item['row'] for item in items)
    assert current_start <= accepted_rows
    first_new = max(0, accepted_rows - current_start)
    new_items = sum(row > first_new for row, _, _, _ in last['cards'])

    first_item = next(item for item in items if item['frame_id'] == frame_ids[0])
    unrelated = load_page(args.session / 'frames' / f'{frame_ids[0]}.png', layout,
                          first_item['box'][1] % layout['row_step'], False)
    negative = locate(previous, unrelated, layout, args.overlap_rows)
    assert not negative['after']['valid']

    result = {'source': str(args.session.resolve()),
              'scope': 'Python/OpenCV离线算法回放；未运行C++、WGC或游戏，不验证跨原始帧稳定及实际输入。',
              'transitions': records, 'unchanged_page': unchanged, 'unrelated_page': negative,
              'continuation': {'at_bottom': last['bottom']['at_bottom'], 'advance': advance,
                               'new_items': new_items, 'accepted_before': len(items),
                               'combined_items': len(items) + new_items}}
    (args.output / 'replay.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    for record in records:
        print(record['previous'], '->', record['current'], 'pixels', record['displacement'],
              'valid', record['before']['valid'], '->', record['after']['valid'],
              'weakest', record['before']['weakest_score'], '->', record['after']['weakest_score'])
    print('continuation', result['continuation'])
    print('unchanged valid/displacement', unchanged['after']['valid'], unchanged['displacement'])
    print('unrelated valid', negative['after']['valid'])


if __name__ == '__main__':
    main()

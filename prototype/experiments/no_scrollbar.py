"""无滚动条首屏的离线适配实验；只读图片，不操作游戏或运行原生程序。"""

import argparse
from copy import deepcopy
from collections import Counter
from html import escape
import json
import math
import os
from pathlib import Path
from time import perf_counter

import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont

from native_resolution import ROOT, Recognizer, detect_bottom, frame_contrast, ownership, samples


EXPECTED_ROWS = [
    (['拂风的翎羽', '晖光的一束', '拂风的翎羽·未萌'], [1, 2]),
    (['蚀骨之痕', '生人回避', '轻云浮忆'], [1, 2]),
    (['轻云浮忆·青岚', '梦尘寰', '济世方'], [2, 3]),
    (['清平调', '有尽时', '梦尘寰·诚本真'], [1, 3]),
    (['露饮瑶池', '如证万相', '琳琅顾影'], [1, 2]),
    (['玉浪探珠', '露饮瑶池·遗音', '隐没簇火'], [2]),
    (['隐没簇火·焰痕', '流光序列', '流光印契'], [2, 3]),
    (['流光序列·新章', '无愿歌', '金庭碎影'], [2]),
    (['织网破蝶', '晨光里', '无愿歌·初光'], [3]),
    (['存于永昼', '存于永昼·重辉', '铭金不朽'], [1]),
    (['铭金不朽·律光'], []),
]


def compare_labels(items):
    expected = {f'r{row:03}_c{column:02}': (name, 'owned' if column in owned else 'unowned')
                for row, (names, owned) in enumerate(EXPECTED_ROWS, 1)
                for column, name in enumerate(names, 1)}
    actual = {item['id']: (item['name']['text'], item['ownership']) for item in items}
    mismatches = [{'id': key, 'expected': expected.get(key), 'actual': actual.get(key)}
                  for key in sorted(expected.keys() | actual.keys()) if expected.get(key) != actual.get(key)]
    return {'method': '逐张查看原图卡片；名称中的分隔符按TXT原文记录。标签不参与识别。',
            'expected_items': len(expected), 'actual_items': len(actual), 'mismatches': mismatches}


def scrollbar_evidence(image, layout):
    settings = layout['bottom_detection']
    pixels = np.asarray(image)
    top = layout['safe_y'][0]
    bottom = settings['line_rect'][3]
    profiles = []
    for key in ('line_rect', 'left_rect', 'right_rect'):
        left, _, right, _ = settings[key]
        profiles.append(pixels[top:bottom, left:right].mean(axis=(1, 2)))
    contrast = profiles[0] - (profiles[1] + profiles[2]) / 2
    length = settings['line_rect'][3] - settings['line_rect'][1]
    fractions = np.convolve((contrast >= settings['min_contrast']).astype(float),
                            np.ones(length) / length, mode='valid')
    best = int(np.argmax(fractions))
    return {'visible': bool(fractions[best] >= settings['min_clear_fraction']),
            'max_clear_fraction': float(fractions[best]), 'best_window_y': top + best,
            'window_height': length, 'contrast_quantiles': np.quantile(contrast, [0, .5, .9, .99, 1]).tolist()}


def grid_evidence(image, layout, phase, at_top):
    gray = cv2.cvtColor(np.asarray(image), cv2.COLOR_RGB2GRAY)
    step = layout['row_step']
    top = layout['safe_y'][0] if at_top else layout['scrolled_safe_top']
    first = phase % step + math.ceil((top - phase % step) / step) * step
    height = layout['card_size'][1]
    slots = []
    for row, y in enumerate(range(first, layout['bottom_detection']['safe_bottom'] - height + 1, step), 1):
        for column, x in enumerate(layout['columns'], 1):
            frame = layout['card_frame']
            scores = [frame_contrast(gray, x, y, frame[key], horizontal)
                      for key, horizontal in [('bottom_rect', True), ('right_rect', False)]]
            edges = [score >= frame['min_contrast'] for score in scores]
            left, top_offset, right, bottom_offset = layout['name_tracking_rect']
            deviation = float(gray[y + top_offset:y + bottom_offset, x + left:x + right].std())
            state = ('present' if deviation >= 3 else 'name_not_ready') if all(edges) else (
                'absent' if not any(edges) else 'conflicting_edges')
            slots.append({'row': row, 'column': column, 'x': x, 'y': y, 'state': state,
                          'bottom_contrast': scores[0], 'right_contrast': scores[1], 'name_std': deviation})
    return slots


def probe(image, layout, phase, at_top=True):
    bottom = detect_bottom(image, layout['bottom_detection'])
    scrollbar = scrollbar_evidence(image, layout)
    slots = grid_evidence(image, layout, phase, at_top)
    first_absent = next((i for i, slot in enumerate(slots) if slot['state'] == 'absent'), len(slots))
    prefix = slots[:first_absent]
    prefix_ready = bool(prefix) and all(slot['state'] == 'present' for slot in prefix)
    tail_empty = first_absent < len(slots) and all(slot['state'] == 'absent' for slot in slots[first_absent:])
    last_row = prefix[-1]['row'] if prefix else 0
    empty_rows = [row for row in sorted({slot['row'] for slot in slots}) if row > last_row
                  and all(slot['state'] == 'absent' and slot['y'] + layout['card_size'][1] <= layout['safe_y'][1]
                          for slot in slots if slot['row'] == row)]
    complete = at_top and not bottom['at_bottom'] and not scrollbar['visible'] and prefix_ready and tail_empty and bool(empty_rows)
    original = {'count': 0, 'error': None}
    try:
        original['count'] = len(list(samples(image, layout, phase, not at_top, bottom)))
    except ValueError as error:
        original['error'] = str(error)
    selected = []
    error = None
    try:
        selected = list(samples(image, layout, phase, not at_top, {**bottom, 'at_bottom': bottom['at_bottom'] or complete}))
    except ValueError as problem:
        error = str(problem)
    return {'bottom': bottom, 'scrollbar': scrollbar, 'single_page_complete': bool(complete),
            'original': original, 'adapted_count': len(selected), 'adapted_error': error,
            'prefix_count': len(prefix), 'full_empty_safe_rows': empty_rows,
            'grid_states': dict(Counter(slot['state'] for slot in slots)), 'slots': slots}, selected


def recognize_items(image, layout, cards, output):
    recognizer = Recognizer(ROOT / 'resources/fonts/FZYASHJW_Zhong.ttf',
                            ROOT / 'resources/clothes_names.txt', layout['name_recognition'],
                            layout['name_rect'][3] - layout['name_rect'][1])
    items = []
    started = perf_counter()
    for row, col, x, y in cards:
        key = f'r{row:03}_c{col:02}'
        rect = layout['name_rect']
        name = recognizer.recognize(image.crop((x + rect[0], y + rect[1], x + rect[2], y + rect[3])))
        s = layout['star_rect']
        state, luma = ownership(image.crop((x + s[0], y + s[1], x + s[2], y + s[3])), layout['ownership_star'])
        image.crop((x, y, x + layout['card_size'][0], y + layout['card_size'][1])).save(output / f'{key}.png')
        issues = [issue for issue in name['issues'] if not (issue.startswith('第') and issue.endswith('字匹配不确定'))]
        items.append({'id': key, 'name': name, 'ownership': state, 'star_luma': luma,
                      'issues_without_character_validation': issues})
        print(key, name['text'], round(name.get('similarity', 0), 5), state, issues, flush=True)
    return {'items': items, 'recognition_seconds': perf_counter() - started,
            'counts': {'items': len(items), 'named': sum(bool(item['name']['text']) for item in items),
                       'review_without_character_validation': sum(bool(item['issues_without_character_validation']) or item['ownership'] == 'unknown' for item in items),
                       'ownership': dict(Counter(item['ownership'] for item in items))}}


def rejection_cases(image, layout):
    def erase(source, row, column, right_edge_only=False):
        changed = source.copy()
        x = layout['columns'][column - 1]
        y = layout['first_row_y'] + (row - 1) * layout['row_step']
        box = [x + 93, y + 30, x + 107, y + 90] if right_edge_only else [x, y, x + 325, y + 110]
        ImageDraw.Draw(changed).rectangle((box[0], box[1], box[2] - 1, box[3] - 1), fill=(210, 190, 160))
        return changed
    cases = [('中间缺格', erase(image, 2, 2), True),
             ('边框只有一条', erase(image, 2, 2, True), True),
             ('整页无卡片', Image.new('RGB', image.size, (210, 190, 160)), True),
             ('滚动后页面不走首屏结束', image, False)]
    results = []
    for title, changed, at_top in cases:
        result, _ = probe(changed, layout, layout['first_row_y'], at_top)
        results.append({'case': title, 'single_page_complete': result['single_page_complete'],
                        'passed': not result['single_page_complete'], 'adapted_error': result['adapted_error']})
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--reference-top', type=Path)
    parser.add_argument('--reference-bottom', type=Path)
    parser.add_argument('--reference-bottom-phase', type=int, default=10)
    parser.add_argument('--columns', type=int, nargs=3, help='当前实验图片的列起点；对照图片仍使用默认布局')
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--recognize', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    layout = json.loads((ROOT / 'resources/layout.json').read_text('utf-8'))
    image = Image.open(args.image).convert('RGB')
    if list(image.size) != layout['reference_size']:
        raise ValueError('原图尺寸与布局不符')
    original_layout = deepcopy(layout)
    unmodified, _ = probe(image, layout, layout['first_row_y'])
    if args.columns:
        layout['columns'] = args.columns
    result, cards = probe(image, layout, layout['first_row_y'])
    result['columns'] = layout['columns']
    result['unmodified_layout'] = {key: value for key, value in unmodified.items() if key != 'slots'}
    result['source'] = str(args.image.resolve())
    result['scope'] = 'Python静态截图实验；未运行C++、WGC、游戏输入，不能证明动态加载及稳定性。'
    result['references'] = []
    for path, phase, at_top in [(args.reference_top, layout['first_row_y'], True),
                                (args.reference_bottom, args.reference_bottom_phase, False)]:
        if path:
            reference, _ = probe(Image.open(path).convert('RGB'), original_layout, phase, at_top)
            result['references'].append({'source': str(path.resolve()), **{key: value for key, value in reference.items() if key != 'slots'}})
    if args.recognize and cards:
        result.update(recognize_items(image, layout, cards, args.output))
        if args.image.name == '20261029_054330_975.jpg':
            result['label_comparison'] = compare_labels(result['items'])
    if cards and result['single_page_complete']:
        result['rejection_cases'] = rejection_cases(image, layout)
    if result.get('items'):
        result['score_minimum'] = min(item['name']['similarity'] for item in result['items'])
        result['margin_minimum'] = min(item['name']['margin'] for item in result['items'])
        result['font_sizes'] = dict(Counter(item['name']['font_size'] for item in result['items']))
    (args.output / 'analysis.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), 'utf-8')
    font = ImageFont.truetype(str(Path(os.environ['WINDIR']) / 'Fonts/msyh.ttc'), 17)
    sheet = Image.new('RGB', (1035, max(1, math.ceil(len(cards) / 3) * 145)), 'white')
    draw = ImageDraw.Draw(sheet)
    items = result.get('items', [])
    for i, (row, col, x, y) in enumerate(cards):
        left, top = (i % 3) * 345, (i // 3) * 145
        sheet.paste(image.crop((x, y, x + 325, y + 110)), (left, top))
        draw.text((left, top + 113), f'{row:02}-{col} ' + (items[i]['name']['text'] if items else ''), fill='black', font=font)
    if cards:
        sheet.save(args.output / 'cards.png')
    page = ['<!doctype html><meta charset="utf-8"><title>无滚动条首屏实验</title>',
            '<style>body{font:16px system-ui;margin:24px}img{max-width:100%}pre{white-space:pre-wrap}</style>',
            '<h1>无滚动条首屏实验</h1><pre>', escape(json.dumps({key: value for key, value in result.items() if key not in ('slots', 'items')}, ensure_ascii=False, indent=2)),
            '</pre><img src="cards.png">']
    for item in items:
        page += [f'<details><summary>{item["id"]} {escape(item["name"]["text"])}</summary><pre>',
                 escape(json.dumps(item, ensure_ascii=False, indent=2)), '</pre></details>']
    (args.output / 'review.html').write_text(''.join(page), 'utf-8')
    print(json.dumps({key: value for key, value in result.items() if key not in ('slots', 'items')}, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()

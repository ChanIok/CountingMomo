"""新分辨率的离线标定：复用原型像素函数，按原生程序的整名评分契约输出证据。

不导入窗口或输入模块，不缩放截图，不调用旧原型的自由认字／图标掩码流程。
"""

import argparse
from collections import Counter
from html import escape
import json
import math
import os
from pathlib import Path
import sys
from time import perf_counter

import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "prototype"))
from font_templates import font_characters, ink_pixels


def gray_vector(ink):
    # 小标点在30px高画布中不足1%；99分位截顶会放大JPEG背景噪声。
    values = ink.astype(np.float32).ravel()
    return values / max(float(np.linalg.norm(values)), 1e-8)


def detect_bottom(image, settings):
    pixels = np.asarray(image)
    regions = []
    for key in ('line_rect', 'left_rect', 'right_rect'):
        x1, y1, x2, y2 = settings[key]
        regions.append(pixels[y1:y2, x1:x2].mean(axis=(1, 2)))
    contrasts = regions[0] - (regions[1] + regions[2]) / 2
    fraction = float(np.mean(contrasts >= settings['min_contrast']))
    return {'at_bottom': fraction >= settings['min_clear_fraction'], 'clear_fraction': fraction,
            'contrast_median': float(np.median(contrasts))}


def frame_contrast(gray, x, y, box, horizontal):
    x1, y1, x2, y2 = box
    roi = gray[y + y1:y + y2, x + x1:x + x2]
    profile = np.median(roi, axis=1 if horizontal else 0)
    return float(profile[2:-2].max() - min(profile[:2].mean(), profile[-2:].mean()))


def samples(image, layout, phase, scrolled, bottom):
    gray = cv2.cvtColor(np.asarray(image), cv2.COLOR_RGB2GRAY)
    step = layout['row_step']
    top = layout['scrolled_safe_top'] if scrolled else layout['safe_y'][0]
    limit = layout['bottom_detection']['safe_bottom'] if bottom['at_bottom'] else layout['safe_y'][1]
    first = phase % step + math.ceil((top - phase % step) / step) * step
    empty_tail = False
    for row, y in enumerate(range(first, limit - layout['card_size'][1] + 1, step), 1):
        for col, x in enumerate(layout['columns'], 1):
            frame = layout['card_frame']
            edges = [frame_contrast(gray, x, y, frame[key], horizontal) >= frame['min_contrast']
                     for key, horizontal in [('bottom_rect', True), ('right_rect', False)]]
            if edges[0] != edges[1] or (not edges[0] and not bottom['at_bottom']):
                raise ValueError(f'卡片边框未就绪 row={row}, column={col}, y={y}, edges={edges}')
            if not edges[0]:
                empty_tail = True
                continue
            if empty_tail:
                raise ValueError('网格中间出现空位')
            tx1, ty1, tx2, ty2 = layout['name_tracking_rect']
            if gray[y + ty1:y + ty2, x + tx1:x + tx2].std() < 3:
                raise ValueError(f'名称尚未就绪 row={row}, column={col}')
            yield row, col, x, y


def ownership(star, settings):
    mask = np.array([[v == '1' for v in row] for row in settings['foreground_mask']])
    pixels = np.asarray(star)
    if pixels.shape[:2] != mask.shape:
        raise ValueError('星星掩码与原生裁图尺寸不符')
    luma = float((pixels[mask] @ np.array([.299, .587, .114])).mean())
    owned = abs(luma - settings['owned_luma']) <= settings['max_luma_error']
    unowned = abs(luma - settings['unowned_luma']) <= settings['max_luma_error']
    return ('owned' if owned else 'unowned') if owned != unowned else 'unknown', luma


def calibrate_stars(image, layout, label_set):
    """首屏前20行选点，y±1px共同前景；不使用第二张图或后20行选择像素。"""
    pixels = np.asarray(image)
    sx1, sy1, sx2, sy2 = layout['star_rect']
    measurements, states = [], []
    for dy in (-1, 0, 1):
        group = []
        for row, labels in enumerate(label_set['rows'][:20]):
            y = label_set['first_y'] + row * layout['row_step']
            for col, x in enumerate(layout['columns'], 1):
                crop = pixels[y + sy1 + dy:y + sy2 + dy, x + sx1:x + sx2]
                group.append(crop @ np.array([.299, .587, .114]))
                if dy == -1:
                    states.append(col in labels['owned_columns'])
        measurements.append(group)
    measurements, states = np.array(measurements), np.array(states)
    mask = measurements[:, states].min(axis=(0, 1)) >= 160
    # 裁图左侧4列属于相邻星星的尖角，不参与最右一颗的采样。
    mask[:, :4] = False
    means = measurements[:, :, mask].mean(axis=2)
    references = []
    for owned in (True, False):
        group = means[:, states == owned]
        references.append(round(float((group.min() + group.max()) / 2), 2))
    return {'foreground_mask': [''.join('1' if p else '0' for p in row) for row in mask],
            'owned_luma': references[0], 'unowned_luma': references[1], 'max_luma_error': 10.0}


class Recognizer:
    def __init__(self, font, names, settings, height):
        self.names = list(dict.fromkeys(n.strip() for n in names.read_text(encoding="utf-8-sig").splitlines()
                                       if n.strip() and n.strip() != "遇上喵喵雨・星彩"))
        self.chars = sorted(set("".join(self.names)))
        if "•" in self.chars:
            raise ValueError("名称库包含不支持的•")
        supported, _ = font_characters(font)
        self.missing = set(self.chars) - set(supported) - {" "}
        self.font_path, self.settings, self.height = font, settings, height
        self.banks = {}

    def bank(self, size):
        if size in self.banks:
            return self.banks[size]
        font = ImageFont.truetype(str(self.font_path), size)
        fallback = ImageFont.truetype(str(Path(os.environ['WINDIR']) / 'Fonts/msyh.ttc'), size, index=0)
        vectors, advances, widths = [], [], []
        for char in self.chars:
            face = fallback if char in self.missing else font
            image = Image.new("L", (size + 1, self.height), 255)
            # 补字与游戏字体共用基线。
            top = self.settings['baseline'] - face.getmetrics()[0]
            ImageDraw.Draw(image).text((.5, top), char, font=face, fill=0)
            vectors.append(gray_vector(ink_pixels(image)).reshape(self.height, size + 1))
            advance = face.getlength(char)
            if not 0 < advance <= size + 1:
                raise ValueError(f"字宽超出模板：{char!r} {advance}")
            advances.append(advance)
            widths.append(int(np.ceil(advance)))
        vectors = np.array(vectors)
        norms = np.sqrt(np.cumsum((vectors ** 2).sum(axis=1), axis=1))
        inverse = np.divide(1, norms, out=np.zeros_like(norms), where=norms > 0)
        energy = vectors.mean(axis=0).sum(axis=0)
        energy[0] += energy[-1]
        indices = {char: i for i, char in enumerate(self.chars)}
        layouts = []
        for name in self.names:
            glyphs = np.array([indices[c] for c in name])
            positions = np.r_[0, np.cumsum(np.array(advances)[glyphs])]
            # C++ lround对非负字宽四舍五入。
            positions = np.floor(positions + .5).astype(int)
            layouts.append((glyphs, positions[:-1], int(positions[-1])))
        row_energy = vectors.mean(axis=0).sum(axis=1)
        bank = (vectors, inverse, np.array(widths), energy[:-1], layouts, row_energy)
        self.banks[size] = bank
        return bank

    def recognize(self, crop, forced_size=None):
        gray = cv2.cvtColor(np.array(crop), cv2.COLOR_RGB2GRAY)
        ink = ink_pixels(gray)
        cols = np.maximum(0., ink.astype(float) - 7.5).sum(axis=0)
        active = np.flatnonzero((cols > 0) & (cols >= cols.max() * .12))
        if not len(active):
            return {'text': '', 'issues': ['未检出可读名称']}
        size = self.settings['font_size_range'][1]
        measured = None
        if forced_size is None and active[-1] - active[0] + 1 >= gray.shape[1] - size:
            rows = np.maximum(0., ink.astype(float) - 7.5).sum(axis=1)
            active_rows = np.flatnonzero((rows > 0) & (rows >= rows.max() * .12))
            measured = int(active_rows[-1] - active_rows[0] + 2)
            minimum, maximum = self.settings['font_size_range']
            # 笔画包围盒不是字号；只在高度邻域比较，再由整名得分选择。
            results = [self.recognize(crop, s) for s in range(max(minimum, measured - 1), min(maximum, measured + 2) + 1)]
            if not results:
                return {'text': '', 'issues': ['文字高度超出当前字号验证范围']}
            result = max(results, key=lambda r: r.get('similarity', -1))
            candidates = {}
            for r in results:
                for c in r.get('candidates', []):
                    candidates[c['text']] = max(candidates.get(c['text'], -1), c['similarity'])
            best = sorted(candidates.items(), key=lambda p: -p[1])[:5]
            if not best:
                return {'text': '', 'issues': ['没有宽度及可见笔画相符的库内名称']}
            result['candidates'] = [{'text': t, 'similarity': v} for t, v in best]
            result['margin'] = best[0][1] - best[1][1] if len(best) > 1 else 0
            result['issues'] = [i for i in result['issues'] if i != '名称库候选差距不足']
            if result['margin'] < .01:
                result['issues'].append('名称库候选差距不足')
            result['text'] = best[0][0] if result['similarity'] >= .9 and result['margin'] >= .01 else ''
            result['measured_size'] = measured
            return result
        if forced_size is not None:
            size = forced_size
        vectors, inverse, widths, energy, layouts, row_energy = self.bank(size)
        # 滚动停靠的像素取整会改变文字相对边框的垂直位置；用整行投影定位，不缩放。
        observed_rows = np.maximum(0., ink.astype(float) - 7.5).sum(axis=1)
        vertical = []
        for dy in (-1, 0, 1):
            expected = np.zeros(self.height)
            source_y, dest_y = max(0, -dy), max(0, dy)
            expected[dest_y:dest_y + self.height - abs(dy)] = row_energy[source_y:source_y + self.height - abs(dy)]
            vertical.append((float(observed_rows @ expected / max(np.linalg.norm(expected), 1e-8)), dy))
        offset_y = max(vertical, key=lambda pair: (pair[0], -abs(pair[1])))[1]
        aligned = np.full_like(gray, 255)
        source_y, dest_y = max(0, offset_y), max(0, -offset_y)
        aligned[dest_y:dest_y + self.height - abs(offset_y)] = gray[source_y:source_y + self.height - abs(offset_y)]
        choices = []
        for origin in range(self.settings['origin_count']):
            x = np.arange(active[-1] + 1)
            expected = np.where(x >= origin, energy[(x - origin) % size], 0)
            choices.append((float(cols[:len(x)] @ expected / np.sqrt(max(expected @ expected, 1e-8))), origin))
        origins = [o for _, o in sorted(choices, key=lambda pair: (-pair[0], pair[1]))[:2]]
        width = gray.shape[1]
        clipped = active[-1] >= width - 3
        requests = [set() for _ in range(width)]
        matches = []
        for ni, (glyphs, lefts, length) in enumerate(layouts):
            for origin in origins:
                end = origin + length
                if end < active[-1] - size // 2 or (not clipped and end > active[-1] + size + 1):
                    continue
                matches.append((ni, origin))
                for left, glyph in zip(lefts + origin, glyphs):
                    if left < width:
                        requests[left].add(int(glyph))
        scores = np.full((width, len(self.chars)), -1., dtype=np.float32)
        for left, requested in enumerate(requests):
            if not requested:
                continue
            cell = np.full((self.height, size + 1), 255, dtype=np.uint8)
            visible = min(size + 1, width - left)
            cell[:, :visible] = aligned[:, left:left + visible]
            query = gray_vector(ink_pixels(cell)).reshape(self.height, size + 1)
            qnorms = np.sqrt(np.cumsum((query ** 2).sum(axis=0)))
            for glyph in requested:
                w = min(int(widths[glyph]), width - left)
                if self.chars[glyph] == ' ':
                    scores[left, glyph] = float(ink[:, left:left + w].max() < 7.5)
                elif inverse[glyph, w - 1] > 0:
                    scores[left, glyph] = float((query[:, :w] * vectors[glyph, :, :w]).sum()
                                               / max(float(qnorms[w - 1]), 1e-8) * inverse[glyph, w - 1])
        ranked = []
        for ni, origin in matches:
            glyphs, lefts, _ = layouts[ni]
            visible = lefts + origin < width
            values = scores[(lefts + origin)[visible], glyphs[visible]]
            values = values[values >= 0]
            if len(values):
                ranked.append((float(values.mean()), ni, origin))
        ranked.sort(key=lambda v: (-v[0], v[1], v[2]))
        best = []
        for match in ranked:
            if all(match[1] != old[1] for old in best):
                best.append(match)
            if len(best) == 5:
                break
        if not best:
            return {'text': '', 'issues': ['没有宽度及可见笔画相符的库内名称']}
        value, ni, origin = best[0]
        margin = value - best[1][0] if len(best) > 1 else 0
        glyphs, lefts, _ = layouts[ni]
        characters, issues = [], []
        if value < .9:
            issues.append('名称整体匹配分数不足')
        if margin < .01:
            issues.append('名称库候选差距不足')
        for i, (glyph, left) in enumerate(zip(glyphs, lefts + origin)):
            score = float(scores[left, glyph]) if left < width else -1
            characters.append({'index': i, 'text': self.chars[glyph], 'left': int(left), 'similarity': max(0., score)})
            if left + widths[glyph] > width:
                issues.append('候选名称超出裁图，后缀无法完整核对')
            if score < 0:
                issues.append('候选名称存在不可见字符')
            elif score < .9:
                issues.append(f'第{i + 1}字匹配不确定')
            if self.chars[glyph] in self.missing:
                issues.append(f'第{i + 1}字使用微软雅黑补字，需核对字形')
        return {'text': self.names[ni] if value >= .9 and margin >= .01 else '', 'similarity': value,
                'margin': margin, 'font_size': size, 'measured_size': measured, 'origin': origin,
                'baseline': self.settings['baseline'] + offset_y,
                'candidates': [{'text': self.names[n], 'similarity': v} for v, n, _ in best],
                'characters': characters, 'issues': list(dict.fromkeys(issues))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--layout', type=Path, default=ROOT / 'resources/layout.json')
    parser.add_argument('--font', type=Path, default=ROOT / 'resources/fonts/FZYASHJW_Zhong.ttf')
    parser.add_argument('--names', type=Path, default=ROOT / 'resources/clothes_names.txt')
    parser.add_argument('--row-phase', type=int, help='滚动截图的行首y对行距取余；首屏省略')
    parser.add_argument('--labels', type=Path, default=Path(__file__).with_name('resolution_1440_labels.json'))
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    started = perf_counter()
    image = Image.open(args.image).convert('RGB')
    layout = json.loads(args.layout.read_text(encoding='utf-8-sig'))
    if list(image.size) != layout['reference_size']:
        raise ValueError('原图尺寸必须与布局相符，不缩放')
    if args.row_phase is not None and not 0 <= args.row_phase < layout['row_step']:
        raise ValueError('行相位必须在0至row_step-1之间')
    n = layout['name_rect']
    recognizer = Recognizer(args.font, args.names, layout['name_recognition'], n[3] - n[1])
    bottom = detect_bottom(image, layout['bottom_detection'])
    items = []
    for row, col, x, y in samples(image, layout, args.row_phase if args.row_phase is not None else layout['first_row_y'],
                                 args.row_phase is not None, bottom):
        item_id = f'r{row:03}_c{col:02}'
        box = [x + n[0], y + n[1], x + n[2], y + n[3]]
        crop = image.crop(box)
        crop.save(args.output / f'{item_id}_name.png')
        item = {'id': item_id, 'box': [x, y, x + layout['card_size'][0], y + layout['card_size'][1]],
                'name_box': box, 'name': recognizer.recognize(crop)}
        image.crop(item['box']).save(args.output / f'{item_id}_card.png')
        s = layout['star_rect']
        star = image.crop((x + s[0], y + s[1], x + s[2], y + s[3]))
        star.save(args.output / f'{item_id}_star.png')
        item['ownership'], item['star_luma'] = ownership(star, layout['ownership_star'])
        items.append(item)
        print(item_id, item['name'].get('font_size'), item['name'].get('text'),
              round(item['name'].get('similarity', 0), 4), item['name']['issues'], flush=True)
    if not items:
        raise ValueError('没有完整可读卡片行')
    seconds = perf_counter() - started
    result = {'source': str(args.image.resolve()), 'layout': layout, 'items': items, 'bottom': bottom,
              'seconds': seconds, 'scope': 'Python离线标定，非C++运行结果；当前原图参与参数标定，非独立准确率评估',
              'counts': {'items': len(items), 'named': sum(bool(i['name']['text']) for i in items),
                         'review': sum(bool(i['name']['issues']) or i['ownership'] == 'unknown' for i in items),
                         'font_sizes': dict(Counter(i['name'].get('font_size') for i in items)),
                         'ownership': dict(Counter(i['ownership'] for i in items))}}
    label_set = next((s for s in json.loads(args.labels.read_text('utf-8'))['screenshots']
                      if s['source'] == args.image.name), None)
    result['label_comparison'] = compare_labels(result, label_set) if label_set else None
    (args.output / 'analysis.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    write_review(result, args.output)
    print(result['counts'], seconds)
    print('逐项标签核对：', result['label_comparison'])


def compare_labels(result, label_set):
    expected = {}
    for row, labels in enumerate(label_set['rows'], 1):
        for col, name in enumerate(labels['names'], 1):
            expected[f'r{row:03}_c{col:02}'] = (name, 'owned' if col in labels['owned_columns'] else 'unowned')
    actual = {i['id']: i for i in result['items']}
    mismatches = []
    for key in sorted(expected.keys() | actual.keys()):
        if key not in actual or key not in expected:
            mismatches.append({'id': key, 'reason': '条目缺失或多出'})
        elif expected[key] != (actual[key]['name']['text'], actual[key]['ownership']):
            mismatches.append({'id': key, 'expected': expected[key],
                               'actual': [actual[key]['name']['text'], actual[key]['ownership']]})
    return {'expected_items': len(expected), 'actual_items': len(actual), 'mismatches': mismatches}


def write_review(result, output):
    page = ['<!doctype html><html lang="zh-CN"><meta charset="utf-8"><title>1440 分辨率离线核对</title>',
            '<style>body{font:16px system-ui;margin:24px;color:#222;background:#f5f5f7}',
            'article{background:white;padding:16px;margin:16px 0}img{vertical-align:middle;margin:8px;image-rendering:pixelated}',
            'details{margin-top:12px}pre{white-space:pre-wrap}</style>',
            '<h1>1440×5760 离线核对</h1>',
            f'<p>{escape(result["source"])}</p><p>{escape(result["scope"])}</p>',
            '<pre>' + escape(json.dumps({'counts': result['counts'], 'bottom': result['bottom'],
                                        'labels': result.get('label_comparison')}, ensure_ascii=False, indent=2)) + '</pre>']
    for item in result['items']:
        key = item['id']
        page += [f'<article><b>{key} · {escape(item["name"]["text"] or "未确定名称")}</b> · {item["ownership"]}',
                 f'<div><img src="{key}_card.png" alt="卡片"><img src="{key}_name.png" width="310" height="60" alt="名称">',
                 f'<img src="{key}_star.png" width="51" height="60" alt="星星"></div>',
                 '<details><summary>完整名称候选、字号与逐字证据</summary><pre>' +
                 escape(json.dumps(item['name'], ensure_ascii=False, indent=2)) + '</pre></details></article>']
    (output / 'review.html').write_text(''.join(page) + '</html>', encoding='utf-8')


if __name__ == '__main__':
    main()

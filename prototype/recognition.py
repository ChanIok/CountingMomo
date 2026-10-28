"""统一名称识别：按行选择字号与起点，再逐字匹配灰度模板。"""

from dataclasses import dataclass
from pathlib import Path
from time import perf_counter

import numpy as np
from PIL import Image

from font_templates import (FontTemplates, LINE_HEIGHT, font_characters, gray_vector,
                            ink_pixels, similarities, template_bank)


DEFAULT_FONT = Path(__file__).resolve().parents[1] / 'temp/FZYASHJW_Zhong.ttf'
MIN_FONT_SIZE, MAX_FONT_SIZE = 9, 13
MIN_INK = 7.5
MIN_SIMILARITY, MIN_MARGIN = .9, .01


@dataclass
class NameRecognizer:
    templates: FontTemplates
    width: int
    icon_box: tuple


def load_recognizer(font_path, layout):
    started = perf_counter()
    font_path = Path(font_path).resolve()
    characters, info = font_characters(font_path)
    left, top, right, bottom = layout['name_rect']
    if bottom - top != LINE_HEIGHT:
        raise ValueError('名称布局需要20px的参考高度。')
    x1, y1, x2, y2 = layout['name_icon_rect']
    recognizer = NameRecognizer(FontTemplates(font_path, characters, info), right - left,
                               (x1 - left, y1 - top, x2 - left, y2 - top))
    template_bank(recognizer.templates, MAX_FONT_SIZE)
    print(f'字体模板已就绪：{len(characters)}个候选字符；字号按名称自动选择。', flush=True)
    return recognizer, {'name_template_load_seconds': perf_counter() - started}


def describe(recognizer):
    return {'method': 'character_gray_templates', 'font': str(recognizer.templates.path),
            **recognizer.templates.info, 'font_size_range': [MIN_FONT_SIZE, MAX_FONT_SIZE],
            'min_similarity': MIN_SIMILARITY, 'min_margin': MIN_MARGIN,
            'score_meaning': '归一化灰度向量余弦相似度，不是概率'}


def empty_name(issue):
    return {'name_text': '', 'name_similarity': 0., 'name_margin': 0.,
            'name_font_size': None, 'name_origin': None, 'name_characters': [],
            'name_issues': [issue]}


def readable_mask(recognizer, width):
    mask = np.ones((LINE_HEIGHT, width), dtype=bool)
    left, top, right, bottom = recognizer.icon_box
    mask[max(0, top):min(LINE_HEIGHT, bottom), max(0, left):min(width, right)] = False
    return mask


def cell_strength(ink, size):
    # 字格保留一列边缘供匹配；占位只取内部，避免把邻字边缘当成新字。
    values = ink[6:17, 1:size].ravel()
    return float(np.partition(values, -5)[-5])


def rank_layout(image, visible_ink, mask, templates, size):
    layouts, queries, cells = [], [], []
    for origin in range(7):
        group = []
        for index, left in enumerate(range(origin, image.width - size, size)):
            visible = visible_ink[:, left:left + size + 1]
            if cell_strength(visible, size) < MIN_INK:
                continue
            crop = image.crop((left, 0, left + size + 1, LINE_HEIGHT))
            group.append(len(cells))
            cells.append({'index': index, 'left': left,
                          'occluded': not bool(mask[:, left:left + size].all()),
                          'weight': float(visible.sum())})
            queries.append(gray_vector(ink_pixels(crop)))
        layouts.append((origin, group))
    if not queries:
        return None
    scores = similarities(queries, template_bank(templates, size))
    best = None
    for origin, group in layouts:
        if not group:
            continue
        quality = float(np.average(scores[group].max(axis=1),
                                   weights=[cells[i]['weight'] for i in group]))
        if best is None or quality > best['quality']:
            best = {'size': size, 'origin': origin, 'quality': quality,
                    'cells': [cells[i] for i in group], 'scores': scores[group].copy()}
    return best


def recognize_name(recognizer, image):
    # 离线截图允许整体缩放，识别在布局参考像素上完成。
    expected = (recognizer.width, LINE_HEIGHT)
    if abs(image.width / image.height - expected[0] / expected[1]) > .1:
        raise ValueError('名称裁图宽高比与当前布局不符。')
    image = image.convert('L').resize(expected, Image.Resampling.LANCZOS)
    mask = readable_mask(recognizer, image.width)
    visible = np.where(mask, ink_pixels(image), 0).astype(np.uint8)
    if float(np.partition(visible.ravel(), -5)[-5]) < MIN_INK:
        return empty_name('未检出可读名称')
    energy = visible.sum(axis=1)
    rows = np.flatnonzero(energy >= .12 * energy.max())
    measured_size = int(rows[-1] - rows[0] + 2)
    rough = int(np.clip(measured_size, MIN_FONT_SIZE, MAX_FONT_SIZE))
    candidates = [rank_layout(image, visible, mask, recognizer.templates, size)
                  for size in range(max(MIN_FONT_SIZE, rough - 1), min(MAX_FONT_SIZE, rough + 1) + 1)]
    candidates = [candidate for candidate in candidates if candidate is not None]
    if not candidates:
        return empty_name('未检出完整名称字格')
    best = max(candidates, key=lambda candidate: candidate['quality'])
    tokens, issues, expected_index = [], [], 0
    if not MIN_FONT_SIZE <= measured_size <= MAX_FONT_SIZE:
        issues.append('文字高度超出当前字号验证范围')
    for cell, scores in zip(best['cells'], best['scores'], strict=True):
        while expected_index < cell['index']:
            tokens.append({'text': '�', 'index': expected_index, 'similarity': 0., 'margin': 0.,
                           'candidates': []})
            issues.append('名称中存在空字格，可能含空格或非等宽混排')
            expected_index += 1
        order = np.argsort(-scores, kind='stable')[:5]
        similarity, margin = float(scores[order[0]]), float(scores[order[0]] - scores[order[1]])
        char = recognizer.templates.characters[order[0]]
        if cell['occluded']:
            issues.append(f"第{cell['index'] + 1}字与图标区域重叠")
        if similarity < MIN_SIMILARITY or margin < MIN_MARGIN:
            issues.append(f"第{cell['index'] + 1}字匹配不确定")
        if recognizer.templates.widths[best['size']][order[0]] != best['size']:
            issues.append('名称可能包含非等宽字符')
        tokens.append({'text': char, 'index': cell['index'], 'left': cell['left'],
                       'similarity': similarity, 'margin': margin,
                       'candidates': [{'text': recognizer.templates.characters[i],
                                       'similarity': float(scores[i])} for i in order]})
        expected_index = cell['index'] + 1
    # 可见笔画触及遮挡或裁图边界时，不能把前缀视为完整名称。
    icon_left = max(0, min(image.width, recognizer.icon_box[0]))
    for boundary, reason in ((icon_left, '名称触及图标区域，可能存在遮挡'),
                             (image.width, '名称触及裁图右边界，可能被截断')):
        if boundary > 1 and float(visible[4:17, boundary - 2:boundary].max()) >= MIN_INK:
            issues.append(reason)
    return {'name_text': ''.join(token['text'] for token in tokens),
            'name_similarity': min(token['similarity'] for token in tokens),
            'name_margin': min(token['margin'] for token in tokens),
            'name_font_size': best['size'], 'name_origin': best['origin'],
            'name_characters': tokens, 'name_issues': list(dict.fromkeys(issues))}


def recognize_names(recognizer, records, images):
    started = perf_counter()
    for record, image in zip(records, images, strict=True):
        record.update(recognize_name(recognizer, image))
    return {'name_recognition_seconds': perf_counter() - started}


def mark_for_review(record):
    reasons = ['名称需要核对：' + issue for issue in record['name_issues']]
    if record['ownership'] == 'unknown':
        reasons.append('拥有状态需要核对')
    record['review_reasons'], record['needs_review'] = reasons, bool(reasons)

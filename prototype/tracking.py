"""以名称带追踪列表，用固定网格裁切名称与最右侧星星。"""

from dataclasses import dataclass
from math import ceil

import cv2
import numpy as np
from PIL import Image

from ownership import sample_star
from scrollbar import detect_bottom
from recognition import empty_name


class PageNotReady(RuntimeError):
    pass


@dataclass
class CardSample:
    record: dict
    card: Image.Image
    name: Image.Image
    star: Image.Image


@dataclass
class CardRow:
    y: int
    cards: list[CardSample]


@dataclass
class Page:
    image: Image.Image
    reference: np.ndarray
    rows: list[CardRow]
    phase: int
    bottom: dict
    names: np.ndarray
    stars: np.ndarray


@dataclass
class NameAnchor:
    y: int
    templates: list[list[np.ndarray]]


def reference_image(image, config):
    width, height = config["reference_size"]
    if abs(image.height - height * image.width / width) > 2:
        raise ValueError("捕获客户区宽高比与布局不符。")
    return np.asarray(image.convert("RGB").resize((width, height)))


def card_frame_contrasts(gray, x, y, config):
    """检测图标底边与右边的亮线；沿边取中位数，避免服装细节和壁纸纹理干扰。"""
    contrasts = []
    for key, axis in (("bottom_rect", 1), ("right_rect", 0)):
        left, top, right, bottom = config[key]
        profile = np.median(gray[y + top:y + bottom, x + left:x + right], axis=axis)
        # 中间带容纳边框的亚像素取整偏移；两端分别为边框内、外侧。
        flank = min(float(np.median(profile[:2])), float(np.median(profile[-2:])))
        contrasts.append(float(profile[2:-2].max()) - flank)
    return contrasts


def read_page(image, config, phase, at_top=False, reference=None, bottom=None):
    reference = reference_image(image, config) if reference is None else reference
    bottom = detect_bottom(image, config) if bottom is None else bottom
    top = config["safe_y"][0] if at_top else config["scrolled_safe_top"]
    limit = config["bottom_detection"]["safe_bottom"] if bottom["at_bottom"] else config["safe_y"][1]
    width, height = config["card_size"]
    step = config["row_step"]
    phase = phase % step
    first_y = phase + ceil((top - phase) / step) * step
    scale = image.width / config["reference_size"][0]
    gray = cv2.cvtColor(reference, cv2.COLOR_RGB2GRAY)
    rows, names, stars = [], [], []
    empty_tail = False

    def scaled(rect):
        return [round(value * scale) for value in rect]

    for y in range(first_y, limit - height + 1, step):
        cards = []
        for column, x in enumerate(config["columns"], 1):
            frame = config["card_frame"]
            present = np.asarray(card_frame_contrasts(gray, x, y, frame)) >= frame["min_contrast"]
            if present.any() and not present.all():
                raise PageNotReady(f"卡片边框尚未就绪（y={y}，第 {column} 列）。")
            if not present.any():
                if not bottom["at_bottom"]:
                    raise PageNotReady(f"卡片边框尚未出现（y={y}，第 {column} 列）。")
                empty_tail = True
                continue
            if empty_tail:
                raise PageNotReady("网格中间出现空位，等待页面刷新。")
            left, y1, right, y2 = config["name_tracking_rect"]
            name_pixels = gray[y + y1:y + y2, x + left:x + right]
            if float(name_pixels.std()) < 3:
                raise PageNotReady(f"名称尚未就绪（y={y}，第 {column} 列）。")
            box = scaled((x, y, x + width, y + height))
            record = {"column": column, "box": box, **empty_name("尚未识别"),
                      "ownership": "unknown", "ownership_basis": "尚未识别",
                      "needs_review": True, "review_reasons": ["尚未识别"]}
            regions = {}
            for kind in ("name", "star"):
                left, y1, right, y2 = config[f"{kind}_rect"]
                rect = scaled((x + left, y + y1, x + right, y + y2))
                record[f"{kind}_box"] = rect
                regions[kind] = image.crop(rect)
            stars.append(sample_star(regions["star"], config["ownership_star"]))
            names.append(name_pixels.copy())
            cards.append(CardSample(record, image.crop(box), regions["name"], regions["star"]))
        if cards:
            rows.append(CardRow(y, cards))
    if not rows:
        raise PageNotReady("没有可读取的完整卡片行。")
    return Page(image, reference, rows, phase, bottom, np.stack(names), np.stack(stars))


def make_anchor(page, config, overlap_rows=6):
    if overlap_rows < 2 or len(page.rows) < overlap_rows:
        raise RuntimeError("完整行不足以选取名称锚点，请检查布局或减小 --overlap-rows。")
    selected = page.rows[-overlap_rows:][:min(3, overlap_rows)]
    gray = cv2.cvtColor(page.reference, cv2.COLOR_RGB2GRAY)
    left, top, right, bottom = config["name_tracking_rect"]
    templates = [[gray[row.y + top:row.y + bottom, x + left:x + right].copy()
                  for x in config["columns"]] for row in selected]
    return NameAnchor(selected[0].y, templates)


def locate_anchor(reference, anchor, config, at_bottom=False):
    gray = cv2.cvtColor(reference, cv2.COLOR_RGB2GRAY)
    top = config["scrolled_safe_top"]
    bottom = config["bottom_detection"]["safe_bottom"] if at_bottom else config["safe_y"][1]
    step = config["row_step"]
    last_y = bottom - config["card_size"][1] - (len(anchor.templates) - 1) * step
    if last_y < top:
        return {"valid": False, "reason": "名称锚点超出安全区"}
    left, name_top, right, _ = config["name_tracking_rect"]
    responses = []
    for index, row in enumerate(anchor.templates):
        for x, template in zip(config["columns"], row, strict=True):
            if float(template.std()) < 3:
                return {"valid": False, "reason": "名称锚点缺少文字特征"}
            response = cv2.matchTemplate(gray[:, x + left:x + right], template, cv2.TM_CCOEFF_NORMED).ravel()
            offset = name_top + index * step
            responses.append(response[top + offset:last_y + offset + 1])
    scores = np.stack(responses)
    average = scores.mean(axis=0)
    best = int(average.argmax())
    alternatives = average[np.abs(np.arange(len(average)) - best) > step // 2]
    runner_up = float(alternatives.max()) if alternatives.size else -1.0
    score, weakest = float(average[best]), float(scores[:, best].min())
    valid = score >= 0.9 and weakest >= 0.8 and score - runner_up >= 0.05
    y = top + best
    return {"valid": valid, "y": y, "displacement": anchor.y - y,
            "score": score, "weakest_score": weakest, "runner_up": runner_up}


def inputs_stable(previous, current):
    previous_slots = [(row.y, len(row.cards)) for row in previous.rows]
    current_slots = [(row.y, len(row.cards)) for row in current.rows]
    if previous_slots != current_slots or previous.bottom["at_bottom"] != current.bottom["at_bottom"]:
        return False
    # 名称使用追踪区域，完整裁图右侧可能包含的图标不参与就绪判断。
    name_diff = np.abs(previous.names.astype(np.float32) - current.names)
    name_error = name_diff.mean(axis=(1, 2))
    star_diff = np.abs(previous.stars - current.stars)
    return bool(np.all(name_error <= 1.0) and np.all(np.percentile(name_diff, 99, axis=(1, 2)) <= 8)
                and np.all(star_diff.mean(axis=(1, 2)) <= 2.0)
                and np.all(np.percentile(star_diff, 99, axis=(1, 2)) <= 8))


def row_advance(previous, current, displacement, config):
    value = (current.rows[0].y - previous.rows[0].y + displacement) / config["row_step"]
    advance = round(value)
    if advance < 0 or abs(value - advance) > 0.02:
        raise RuntimeError("名称锚点位移与固定网格不一致，停止以避免错误接续。")
    return advance


def assign_records(rows, global_start, frame_id, category, frame_row_offset=0):
    samples = []
    for local_row, row in enumerate(rows, 1):
        global_row = global_start + local_row
        for sample in row.cards:
            record = sample.record
            item_id = f"r{global_row:03d}_c{record['column']:02d}"
            record.update(id=item_id, row=global_row, frame_id=frame_id, frame_row=local_row + frame_row_offset,
                          source=f"frames/{frame_id}.jpg", category=category)
            for kind in ("card", "name", "star"):
                record[f"{kind}_crop"] = f"crops/{item_id}_{kind}.png"
            samples.append(sample)
    return samples

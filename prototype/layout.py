"""按照手动标定的网格裁切；不猜测不同宽高比下的游戏布局。"""

import json
import math
from pathlib import Path

from PIL import Image

from ownership import star_mask


def load_layout(layout_path: Path):
    config = json.loads(layout_path.read_text(encoding="utf-8-sig"))
    ref_width, ref_height = config["reference_size"]
    width, height = config["card_size"]
    top, bottom = config["safe_y"]
    first_y, step = config["first_row_y"], config["row_step"]
    if ref_width <= 0 or ref_height <= 0 or width <= 0 or height <= 0 or step < height or not 0 <= top < bottom <= ref_height:
        raise ValueError("card_size、row_step 或 safe_y 配置无效。")
    if not top <= first_y <= bottom - height or not config["columns"]:
        raise ValueError("first_row_y 或 columns 配置无效。")
    if config["columns"] != sorted(set(config["columns"])):
        raise ValueError("columns 必须从左到右且不重复。")
    if any(not 0 <= x < x + width <= ref_width for x in config["columns"]):
        raise ValueError("columns 中的卡片超出截图宽度。")
    for key in ("name_rect", "name_tracking_rect", "name_icon_rect", "star_rect"):
        left, y1, right, y2 = config[key]
        if not 0 <= left < right <= width or not 0 <= y1 < y2 <= height:
            raise ValueError(f"{key} 必须在卡片内部。")
    frame = config["card_frame"]
    for key, axis in (("bottom_rect", 1), ("right_rect", 0)):
        left, y1, right, y2 = frame[key]
        if not 0 <= left < right <= width or not 0 <= y1 < y2 <= height:
            raise ValueError(f"card_frame.{key} 必须在卡片内部。")
        if (y2 - y1 if axis == 1 else right - left) < 5:
            raise ValueError(f"card_frame.{key} 必须容纳中间边框带与两侧各2像素。")
    if not math.isfinite(frame["min_contrast"]) or not 0 < frame["min_contrast"] <= 255:
        raise ValueError("card_frame.min_contrast 必须在0–255内且大于0。")
    star = config["ownership_star"]
    left, y1, right, y2 = config["star_rect"]
    if star_mask(star).shape != (y2 - y1, right - left):
        raise ValueError("星形前景掩码尺寸必须与 star_rect 一致。")
    owned, unowned, error = (float(star[key]) for key in ("owned_luma", "unowned_luma", "max_luma_error"))
    if not all(math.isfinite(value) for value in (owned, unowned, error)) or not 0 <= unowned < owned <= 255 or not 0 < error < (owned - unowned) / 2:
        raise ValueError("星星参考亮度须在0–255内，已拥有高于未拥有，两组误差范围不得重叠。")
    if "scroll_rect" in config:
        left, y1, right, y2 = config["scroll_rect"]
        if not 0 <= left < right <= ref_width or not 0 <= y1 < y2 <= ref_height:
            raise ValueError("scroll_rect 必须在参考画面内部。")
    if "bottom_detection" in config:
        if not top <= config["scrolled_safe_top"] < bottom - height:
            raise ValueError("scrolled_safe_top 必须在安全列表区域内。")
        if not bottom <= config["bottom_detection"]["safe_bottom"] <= ref_height:
            raise ValueError("到底后的安全下边界必须在原下边界与画面底部之间。")
    return config


def extract_cards(image: Image.Image, layout_path: Path, output: Path):
    config = load_layout(layout_path)
    ref_width, ref_height = config["reference_size"]
    scale = image.width / ref_width
    if abs(image.height - ref_height * scale) > 2:
        raise ValueError("截图宽高比与布局配置不符，请重新标定布局，不要拉伸截图。")
    width, height = config["card_size"]
    top, bottom = config["safe_y"]
    first_y, step = config["first_row_y"], config["row_step"]

    def scaled_box(box):
        return [round(value * scale) for value in box]

    records = []
    crops_path = output / "crops"
    crops_path.mkdir()
    row, y = 1, first_y
    while y + height <= bottom:
        if y >= top:
            for column, x in enumerate(config["columns"], start=1):
                if not 0 <= x < x + width <= ref_width:
                    raise ValueError("columns 中的卡片超出截图宽度。")
                item_id = f"r{row:03d}_c{column:02d}"
                box = scaled_box([x, y, x + width, y + height])
                if box[2] <= box[0] or box[3] <= box[1]:
                    raise ValueError("截图尺寸过小，裁切区域为空。")
                item = {"id": item_id, "row": row, "column": column, "box": box}
                image.crop(box).save(crops_path / f"{item_id}_card.png")
                item["card_crop"] = f"crops/{item_id}_card.png"
                for kind in ("name", "star"):
                    left, y1, right, y2 = config[f"{kind}_rect"]
                    crop_box = scaled_box([x + left, y + y1, x + right, y + y2])
                    image.crop(crop_box).save(crops_path / f"{item_id}_{kind}.png")
                    item[f"{kind}_box"] = crop_box
                    item[f"{kind}_crop"] = f"crops/{item_id}_{kind}.png"
                records.append(item)
        y += step
        row += 1
    if not records:
        raise ValueError("布局配置未产生任何完整卡片。")
    return records, config

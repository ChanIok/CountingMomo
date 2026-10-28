"""用滚动条底端亮线相对相邻背景的对比度识别列表到底。"""

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

from layout import load_layout


def detect_bottom(image, config):
    settings = config["bottom_detection"]
    if not np.isfinite(settings["min_contrast"]) or settings["min_contrast"] <= 0:
        raise ValueError("滚动条最小对比度必须是有限正数。")
    if not 0 < settings["min_clear_fraction"] <= 1:
        raise ValueError("滚动条清晰行比例阈值必须在 (0,1] 内。")
    width, height = config["reference_size"]
    if abs(image.height - height * image.width / width) > 2:
        raise ValueError("滚动条检测画面宽高比与布局不符。")
    reference = image.convert("RGB").resize((width, height))
    regions = {}
    for name in ("line", "left", "right"):
        left, top, right, bottom = settings[f"{name}_rect"]
        if not 0 <= left < right <= width or not 0 <= top < bottom <= height:
            raise ValueError("滚动条采样区域超出布局。")
        pixels = np.asarray(reference.crop((left, top, right, bottom)), dtype=np.float32)
        regions[name] = pixels.mean(axis=2).mean(axis=1)
    if len({len(values) for values in regions.values()}) != 1:
        raise ValueError("滚动条与左右背景的采样高度必须相同。")
    # 相邻背景抵消页面纵向渐变；要求整段底端亮线可见，避开单点装饰干扰。
    contrast = regions["line"] - (regions["left"] + regions["right"]) / 2
    fraction = float(np.mean(contrast >= settings["min_contrast"]))
    return {"at_bottom": fraction >= settings["min_clear_fraction"],
            "clear_fraction": fraction, "contrast_median": float(np.median(contrast)),
            "contrast_min": float(contrast.min()), "contrast_max": float(contrast.max())}


def main():
    parser = argparse.ArgumentParser(description="分析本地截图的滚动条底端，不启动 WGC 或发送输入。")
    parser.add_argument("images", nargs="+", type=Path)
    parser.add_argument("--layout", type=Path, default=Path(__file__).parent / "layouts/portrait_1_4.json")
    parser.add_argument("--output", type=Path, help="可选：新建目录，保存逐图采样预览及指标")
    args = parser.parse_args()
    config = load_layout(args.layout)
    if args.output:
        args.output.mkdir(parents=True, exist_ok=False)
    results = []
    for index, path in enumerate(args.images, 1):
        with Image.open(path) as source:
            image = source.convert("RGB")
        result = {"source": str(path.resolve()), **detect_bottom(image, config)}
        results.append(result)
        print(json.dumps(result, ensure_ascii=False))
        if args.output:
            preview = image.resize(tuple(config["reference_size"]))
            draw = ImageDraw.Draw(preview)
            for name, color in (("line", "red"), ("left", "blue"), ("right", "blue")):
                draw.rectangle(config["bottom_detection"][f"{name}_rect"], outline=color, width=1)
            preview.crop((900, 3660, 960, 3810)).resize((240, 600)).save(args.output / f"{index:02d}_{path.stem}.png")
    if args.output:
        (args.output / "measurements.json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()

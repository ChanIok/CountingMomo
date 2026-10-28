"""CountingMomo 离线截图扫描入口。"""

import argparse
import json
from datetime import datetime
from pathlib import Path
from time import perf_counter

from PIL import Image, ImageOps

from layout import extract_cards
from report import write_reports
from recognition import DEFAULT_FONT, describe, empty_name, load_recognizer, mark_for_review, recognize_names


def positive_int(text):
    value = int(text)
    if value <= 0:
        raise argparse.ArgumentTypeError("必须是正整数")
    return value


def main():
    root = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description="扫描无限暖暖图鉴截图，识别名称并通过最右侧星星前景亮度判断拥有状态。")
    parser.add_argument("image", type=Path, help="原始截图路径")
    parser.add_argument("--category", required=True, help="手动指定类别，例如：套装、发型")
    parser.add_argument("--layout", type=Path, default=root / "layouts/portrait_1_4.json")
    parser.add_argument("--output", type=Path, help="新建结果目录；不覆盖已有目录")
    parser.add_argument("--layout-only", action="store_true", help="只裁切并生成报告，不运行识别")
    parser.add_argument("--font", type=Path, default=DEFAULT_FONT, help="游戏TrueType字体文件")
    args = parser.parse_args()
    total_started = started = perf_counter()
    if not args.image.is_file():
        parser.error(f"截图不存在：{args.image}")
    output = args.output or root / "results" / datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    if output.exists():
        parser.error(f"结果目录已存在，请更换 --output：{output}")

    with Image.open(args.image) as source:
        image = ImageOps.exif_transpose(source).convert("RGB")
    timings = {"image_load_seconds": perf_counter() - started}
    output.mkdir(parents=True)
    started = perf_counter()
    records, layout = extract_cards(image, args.layout, output)
    timings["crop_seconds"] = perf_counter() - started
    print(f"截图 {image.width}×{image.height}，切分 {len(records)} 张完整卡片。", flush=True)
    for item in records:
        item.update(source=str(args.image.resolve()), category=args.category)

    if args.layout_only:
        for item in records:
            item.update(**empty_name("仅校准布局"),
                        ownership="unknown", ownership_basis="未运行识别",
                        needs_review=True, review_reasons=["仅校准布局"])
    else:
        from ownership import detect_ownership

        def star_samples():
            for item in records:
                with Image.open(output / item["star_crop"]) as crop:
                    yield crop.convert("RGB")

        timings.update(detect_ownership(records, star_samples(), layout["ownership_star"]))
        recognizer, loading = load_recognizer(args.font, layout)
        timings.update(loading)

        def name_images():
            for item in records:
                with Image.open(output / item["name_crop"]) as crop:
                    yield crop.convert("RGB")

        timings.update(recognize_names(recognizer, records, name_images()))
        for item in records:
            mark_for_review(item)

    metadata = {
        "created_at": datetime.now().astimezone().isoformat(),
        "source": str(args.image.resolve()), "image_size": list(image.size),
        "category": args.category, "layout": layout, "layout_only": args.layout_only,
        "recognition": None if args.layout_only else describe(recognizer),
        "ownership_method": "rightmost_star_foreground_luma" if not args.layout_only else None,
        "scope": "single_screenshot_safe_region", "timings": timings,
    }
    # 总耗时包含导入依赖，报告写盘耗时另行在终端显示。
    timings["before_export_seconds"] = perf_counter() - total_started
    export_started = perf_counter()
    write_reports(image, records, metadata, output)
    print(f"导出 {perf_counter() - export_started:.2f} 秒。", flush=True)
    print(json.dumps(metadata["counts"], ensure_ascii=False), flush=True)
    print(f"核对报告：{(output / 'review.html').resolve()}", flush=True)


if __name__ == "__main__":
    main()

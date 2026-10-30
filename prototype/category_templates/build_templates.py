"""从原始截图重新裁出菜单图标；不连接游戏、不发送输入。"""

import argparse
import json
from pathlib import Path

from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("screenshots", type=Path, help="SpinningMomo 原始截图目录")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    sources = {"screenshots": args.screenshots, "results": root.parent / "results"}
    config = json.loads((root / "categories.json").read_text(encoding="utf-8"))
    for group in config["groups"]:
        entries = ([group["entry"]] if "entry" in group else []) + group["categories"]
        for entry in entries:
            source = sources[entry.get("source_root", "screenshots")] / entry["source"]
            with Image.open(source) as image:
                if image.size != tuple(config["reference_size"]):
                    raise ValueError(f"截图尺寸不符：{entry['source']} {image.size}")
                image.crop(entry["crop"]).convert("RGB").save(root / entry["template"])
            print(entry["template"])


if __name__ == "__main__":
    main()

"""Extract an ordered, deduplicated UTF-8 name library from clothes_all.json."""

import argparse
import json
from pathlib import Path


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=root / "clothes_all.json")
    parser.add_argument("--output", type=Path, default=root / "resources/clothes_names.txt")
    args = parser.parse_args()
    clothes = json.loads(args.input.read_text(encoding="utf-8-sig"))["clothes"]
    names = dict.fromkeys(item["name"].strip() for item in clothes)
    names.pop("", None)
    if not names:
        raise ValueError("clothes 中没有可用名称")
    # 仅清除首尾空白；内部空格、标点和疑似占位名称保留原样。
    if any("\n" in name or "\r" in name for name in names):
        raise ValueError("名称含换行，无法写入一行一个名称的 TXT")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(names) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(clothes)} 条记录 → {len(names)} 个名称：{args.output}")


if __name__ == "__main__":
    main()

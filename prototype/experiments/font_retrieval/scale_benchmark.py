"""两条字号不同的现场名称，共用正式识别器进行离线核对。"""

import argparse
import json
from pathlib import Path
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from layout import load_layout
from recognition import DEFAULT_FONT, describe, load_recognizer, recognize_name


def run(args):
    config = load_layout(args.layout)
    recognizer, loading = load_recognizer(args.font, config)
    with Image.open(args.image) as source:
        if source.size != tuple(config['reference_size']):
            raise ValueError('现场截图尺寸与布局不符。')
        left, top, right, bottom = config['name_rect']
        row_y = config['first_row_y']
        images = [source.convert('RGB').crop((x + left, row_y + top, x + right, row_y + bottom))
                  for x in config['columns'][:2]]
    rows = [recognize_name(recognizer, image) for image in images]
    report = {'image': str(args.image), 'recognition': describe(recognizer), 'loading': loading,
              'scope': '此现场只核对共享前缀于日落再相见，后缀末字受图标遮挡，不能作为完整名称标签。',
              'predictions': rows}
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf8')
    font = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 18)
    sheet = Image.new('RGB', (1200, 460), 'white')
    draw = ImageDraw.Draw(sheet)
    for i, (image, row) in enumerate(zip(images, rows, strict=True)):
        top = i * 225
        draw.text((10, top), f"字号{row['name_font_size']}，起点{row['name_origin']}：{row['name_text']}", font=font, fill='black')
        sheet.paste(image.resize((684, 120), Image.Resampling.NEAREST), (10, top + 30))
        draw.text((10, top + 160), '；'.join(row['name_issues']) or '未发现需核对项', font=font, fill='#b42318')
    sheet.save(args.output / 'comparison.png')
    print(json.dumps(rows, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=Path(r'C:\Users\CH\Videos\SpinningMomo\20261024_045048_626.jpg'))
    parser.add_argument('--font', type=Path, default=DEFAULT_FONT)
    parser.add_argument('--layout', type=Path, default=ROOT / 'layouts/portrait_1_4.json')
    parser.add_argument('--output', type=Path, default=ROOT / 'results/name_scale_validation')
    run(parser.parse_args())

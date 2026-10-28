"""用已有标注核对正式名称识别模块；不维护第二份识别算法。"""

import argparse
from html import escape
import json
from pathlib import Path
import sys
from time import perf_counter

from PIL import Image

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT))
from layout import load_layout
from recognition import DEFAULT_FONT, describe, load_recognizer, recognize_name


def edit_distance(a, b):
    row = list(range(len(b) + 1))
    for i, left in enumerate(a, 1):
        updated = [i]
        for j, right in enumerate(b, 1):
            updated.append(min(updated[-1] + 1, row[j] + 1, row[j - 1] + (left != right)))
        row = updated
    return row[-1]


def metrics(rows):
    accepted = [row for row in rows if not row['prediction']['name_issues']]
    return {'names': len(rows),
            'correct': sum(row['text'] == row['prediction']['name_text'] for row in rows),
            'edit_errors': sum(edit_distance(row['text'], row['prediction']['name_text']) for row in rows),
            'characters': sum(len(row['text']) for row in rows),
            'accepted': len(accepted),
            'accepted_wrong': sum(row['text'] != row['prediction']['name_text'] for row in accepted),
            'saved_ocr_correct': sum(row['text'] == row['ocr_text'].strip() for row in rows)}


def run(args):
    manifest = json.loads(args.samples.read_text(encoding='utf8'))
    samples = manifest['samples']
    if any(not sample.get('reviewed') or not sample.get('text') for sample in samples):
        raise ValueError('所有样本都需要核对标签。')
    groups = {}
    for sample in samples:
        if sample['split'] not in ('calibration', 'evaluation'):
            raise ValueError('split必须是calibration或evaluation。')
        text = sample['text'].strip()
        if text in groups and groups[text] != sample['split']:
            raise ValueError('同一名称不能跨标定与评估分组。')
        groups[text] = sample['split']
    config = load_layout(args.layout)
    images = []
    # 旧名称裁图只有108px，必须从原卡片重新裁切，不能拉伸成114px。
    for sample in samples:
        name_path = ROOT / sample['crop']
        if not name_path.name.endswith('_name.png'):
            raise ValueError('样本路径必须指向原始_name.png裁图。')
        card_path = name_path.with_name(name_path.name[:-len('_name.png')] + '_card.png')
        with Image.open(card_path) as card:
            if card.size != tuple(config['card_size']):
                raise ValueError(f'原卡片尺寸与布局不符：{card_path}')
            images.append(card.convert('RGB').crop(config['name_rect']))
    recognizer, loading = load_recognizer(args.font, config)
    started = perf_counter()
    rows = [{key: sample[key] for key in ('id', 'text', 'split', 'column', 'ocr_text')}
            for sample in samples]
    for row, image in zip(rows, images, strict=True):
        row['text'] = row['text'].strip()
        row['prediction'] = recognize_name(recognizer, image)
    elapsed = perf_counter() - started
    report = {'recognition': describe(recognizer), 'loading': loading,
              'label_source': manifest['label_source'], 'prediction_seconds': elapsed,
              'seconds_per_120': elapsed * 120 / len(rows),
              'scope': '复用探索阶段的辅助标注；当前模块无参数训练，本次未使用标签选择字号或字符。',
              'groups': {split: metrics([row for row in rows if row['split'] == split])
                         for split in ('calibration', 'evaluation')}, 'predictions': rows}
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf8')
    table = []
    for row, image in zip(rows, images, strict=True):
        name = row['id'].replace(':', '_') + '.png'
        image.save(args.output / name)
        p = row['prediction']
        style = 'bad' if p['name_text'] != row['text'] else 'review' if p['name_issues'] else 'ok'
        table.append(f'<tr class="{style}"><td>{escape(row["id"])}</td><td><img src="{escape(name)}"></td>'
                     f'<td>{escape(row["text"])}</td><td>{escape(p["name_text"])}</td>'
                     f'<td>{p["name_font_size"]}</td><td>{escape("；".join(p["name_issues"]))}</td></tr>')
    html = ('<!doctype html><meta charset="utf-8"><title>正式名称模块离线核对</title>'
            '<style>body{font:16px system-ui;margin:24px}td,th{padding:8px;border-bottom:1px solid #ddd}'
            'img{width:456px;image-rendering:pixelated}.bad{background:#fee2e2}.review{background:#fef9c3}</style>'
            '<h1>正式名称模块离线核对</h1><p>与实时扫描共用识别器。原卡片重新裁为114px宽；标签为助手辅助核对。</p>'
            '<table><tr><th>样本</th><th>裁图</th><th>标签</th><th>输出</th><th>字号</th><th>核对原因</th></tr>'
            + ''.join(table) + '</table>')
    (args.output / 'review.html').write_text(html, encoding='utf8')
    print(json.dumps({**report['groups'], 'seconds_per_120': report['seconds_per_120']}, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--font', type=Path, default=DEFAULT_FONT)
    parser.add_argument('--layout', type=Path, default=ROOT / 'layouts/portrait_1_4.json')
    parser.add_argument('--samples', type=Path, default=HERE / 'samples.json')
    parser.add_argument('--output', type=Path, default=ROOT / 'results/name_recognition')
    run(parser.parse_args())

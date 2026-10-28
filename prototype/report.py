"""生成本地离线报告，保留原始证据供人工核对。"""

import csv
import html
import json
from collections import Counter, defaultdict
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from ownership import star_mask


def preview_image(image, records, target):
    font_path = Path("C:/Windows/Fonts/msyh.ttc")
    font = ImageFont.truetype(str(font_path), 13) if font_path.exists() else ImageFont.load_default()
    preview = image.copy()
    draw = ImageDraw.Draw(preview)
    colors = {"owned": "#15803d", "unowned": "#b91c1c", "unknown": "#a16207"}
    for item in records:
        color = colors[item["ownership"]]
        draw.rectangle(item["box"], outline=color, width=2)
        draw.rectangle(item["name_box"], outline="#2563eb", width=1)
        draw.rectangle(item["star_box"], outline="#9333ea", width=1)
        x, _, right, bottom = item["box"]
        draw.rectangle([x, bottom - 20, right, bottom], fill="#fffaf0")
        draw.text((x + 2, bottom - 18), f"{item['id']} {item['name_text']}", fill=color, font=font)
    preview.save(target, quality=95)


def write_reports(image, records, metadata, output):
    counts = Counter(item["ownership"] for item in records)
    metadata["counts"] = {key: counts[key] for key in ("owned", "unowned", "unknown")}
    metadata["card_count"] = len(records)
    metadata["review_count"] = sum(item["needs_review"] for item in records)
    metadata["name_review_count"] = sum(bool(item['name_issues']) for item in records)
    (output / "items.json").write_text(
        json.dumps({"metadata": metadata, "items": records}, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    fields = [
        "id", "source", "category", "row", "column", "frame_id", "frame_row", "name_text",
        "name_similarity", "name_margin", "name_font_size", "name_origin", "name_issues",
        "ownership", "ownership_basis", "star_luma", "star_pixel_count", "star_owned_error", "star_unowned_error", "needs_review",
        "review_reasons", "card_crop", "name_crop", "star_crop",
    ]
    with (output / "items.csv").open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        for item in records:
            # Excel 直接打开 CSV 时，不将识别文字当作公式执行；JSON 保留原文。
            row = {key: item.get(key, "") for key in fields}
            row["review_reasons"] = "；".join(item["review_reasons"])
            row['name_issues'] = '；'.join(item['name_issues'])
            for key, value in row.items():
                if isinstance(value, str) and value.lstrip().startswith(("=", "+", "-", "@")):
                    row[key] = "'" + value
            writer.writerow(row)

    colors = {"owned": "#15803d", "unowned": "#b91c1c", "unknown": "#a16207"}
    labels = {"owned": "已拥有", "unowned": "未拥有", "unknown": "待确认"}
    preview_links = []
    if image is not None:
        preview_image(image, records, output / "preview.jpg")
        preview_links.append('<a href="preview.jpg">卡片定位预览</a>')
    else:
        (output / "previews").mkdir(exist_ok=True)
        grouped = defaultdict(list)
        for item in records:
            grouped[item["frame_id"]].append(item)
        for frame_id, frame_records in grouped.items():
            source = output / frame_records[0]["source"]
            if source.is_file():
                with Image.open(source) as frame:
                    preview_image(frame.convert("RGB"), frame_records, output / f"previews/{frame_id}.jpg")
                preview_links.append(f'<a href="previews/{frame_id}.jpg">{html.escape(frame_id)}</a>')

    rows = []
    escape = lambda value: html.escape(str(value), quote=True)
    for item in records:
        candidates = []
        for token in item['name_characters']:
            choices = ' / '.join(f"{candidate['text']} {candidate['similarity']:.3f}"
                                 for candidate in token['candidates'])
            candidates.append(f"第{token['index'] + 1}字：{choices or '空字格'}")
        evidence = '<details><summary>逐字候选</summary>' + '<br>'.join(escape(value) for value in candidates) + '</details>' if candidates else ''
        star_evidence = "未运行判定"
        if "star_luma" in item:
            star_evidence = (
                f"{item['star_pixel_count']} 个前景像素，平均亮度 {item['star_luma']:.2f}<br>"
                f"已拥有亮度差 {item['star_owned_error']:.2f}<br>"
                f"未拥有亮度差 {item['star_unowned_error']:.2f}"
            )
        rows.append(f"""<tr data-review="{str(item['needs_review']).lower()}">
<td>{escape(item['id'])}<br>{escape(item.get('frame_id', ''))}</td>
<td><a href="{escape(item['card_crop'])}"><img class="card" src="{escape(item['card_crop'])}" loading="lazy"></a></td>
<td><small>原始区域</small><br><img class="line" src="{escape(item['name_crop'])}" loading="lazy"><br><b>{escape(item['name_text'])}</b><br>最小字符相似度 {item['name_similarity']:.4f}<br>最小候选差距 {item['name_margin']:.4f}<br>字号 {escape(item['name_font_size'])}，起点 {escape(item['name_origin'])}{evidence}</td>
<td><small>最右侧星星</small><br><img class="star" src="{escape(item['star_crop'])}" loading="lazy"><br>{star_evidence}</td>
<td style="color:{colors[item['ownership']]}">{labels[item['ownership']]}<br>{escape(item['ownership_basis'])}</td>
<td>{escape('；'.join(item['review_reasons']))}</td></tr>""")
    document = """<!doctype html><html lang="zh-CN"><meta charset="utf-8">
<title>CountingMomo 识别核对</title>
<style>body{font:15px system-ui;margin:24px;background:#faf8f4;color:#292524}
table{border-collapse:collapse;width:100%}th,td{padding:10px;border:1px solid #d6d3d1;text-align:left}
th{background:#e7e5e4}img.card{width:326px;max-width:100%}img.line{height:46px;max-width:100%;object-fit:contain;object-position:left}
img.star{height:100px;image-rendering:pixelated}
button{padding:8px 14px;margin:12px 0}body.review-only tr[data-review="false"]{display:none}td{vertical-align:top}</style>
<h1>CountingMomo 识别核对</h1>
<p>本报告覆盖本次采集并成功接续的卡片。名称来自游戏字体的逐字灰度匹配，不使用名称库；相似度和候选差距都不是概率或准确率。低匹配分数、同分、图标区域重叠及异常字格均需核对。拥有状态来自最右侧星星前景的平均亮度，与名称识别独立。</p>
<p>仅统计星形掩码内的前景像素，不统计矩形背景。亮度差越小越接近参考值，未匹配任一范围时待确认。其他 UI 缩放或色彩设置需要重新核对。</p>
SUMMARY
<p><a href="items.csv">CSV</a> · <a href="items.json">JSON</a></p>
<p>定位预览：PREVIEWS</p>
<button onclick="document.body.classList.toggle('review-only')">切换：仅需核对 / 全部</button>
<table><thead><tr><th>位置</th><th>原始卡片</th><th>名称</th><th>星星采样（紫框）</th><th>拥有状态</th><th>核对原因</th></tr></thead><tbody>
ROWS
</tbody></table></html>"""
    summary = f"<p>类别：{escape(metadata['category'])}；卡片 {len(records)} 张；需核对 {metadata['review_count']} 张；其中名称需核对 {metadata['name_review_count']} 张。</p>"
    if "session" in metadata:
        session = metadata["session"]
        summary += f"<p><strong>扫描状态：{escape(session['status'])}；{escape(session['message'])}</strong></p>"
        summary += "<p>请确认扫描从分类顶部开始；到底信号来自滚动条底端亮线。未确认本轮滚动响应时停止，不继续追加输入。提供分类总数还可以核对采集数量。</p>"
    summary += f"<p>拥有状态：{escape(json.dumps(metadata['counts'], ensure_ascii=False))}</p>"
    star_config = metadata["layout"]["ownership_star"]
    foreground = star_mask(star_config)
    mask_preview = Image.fromarray(foreground.astype("uint8") * 255)
    mask_preview.resize((foreground.shape[1] * 10, foreground.shape[0] * 10), Image.Resampling.NEAREST).save(output / "star_foreground_mask.png")
    summary += (
        f"<p>已拥有参考亮度 {star_config['owned_luma']}；"
        f"未拥有参考亮度 {star_config['unowned_luma']}；"
        f"允许亮度差 ≤ {star_config['max_luma_error']}（0–255）。</p>"
        f'<p>实际采样 {int(foreground.sum())} 个前景像素（白色）：<br><img class="star" src="star_foreground_mask.png"></p>'
    )
    summary += f"<pre>{escape(json.dumps(metadata['timings'], ensure_ascii=False, indent=2))}</pre>"
    (output / "review.html").write_text(
        document.replace("SUMMARY", summary).replace("PREVIEWS", " · ".join(preview_links)).replace("ROWS", "\n".join(rows)), encoding="utf-8"
    )

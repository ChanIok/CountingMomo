"""已有裁图上的离线检索对照；结果独立保存，不修改扫描报告。"""

import argparse
from collections import Counter
from dataclasses import asdict
from difflib import SequenceMatcher
from html import escape
import json
import os
from pathlib import Path
import random
import sys
from time import perf_counter

# 小矩阵检索使用一个计算线程；不让 BLAS 为每条短名称启动大量线程。
os.environ["OPENBLAS_NUM_THREADS"] = "1"
import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont

from templates import (Raster, TemplateBank, font_characters, gray_features, ink_image, render)


HERE = Path(__file__).resolve().parent
PROTOTYPE = HERE.parents[1]
PROJECT = PROTOTYPE.parent
SOURCES = [
    ("live_20261024_033018_762642", "hair", 120),
    ("live_20261024_023946_053174", "set", 50),
    ("scan_neckwear_wider_names", "accessory.choker", 30),
]


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def write_json(path, data):
    Path(path).write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def catalog_entries(path):
    return read_json(path)["clothes"]


def load_samples(path):
    data = read_json(path)
    samples = data["samples"]
    for sample in samples:
        if not sample.get("reviewed") or not sample.get("text"):
            raise ValueError(f"样本 {sample['id']} 尚未逐图核对，不能计入准确率。")
    splits = {}
    for sample in samples:
        if sample["text"] in splits and splits[sample["text"]] != sample["split"]:
            raise ValueError("相同名称同时出现在标定和评估集。")
        splits[sample["text"]] = sample["split"]
    if set(splits.values()) != {"calibration", "evaluation"}:
        raise ValueError("需要非空标定集和评估集，且 split 只能取这两个值。")
    return samples


def prepare(args):
    if args.samples.exists():
        raise ValueError("标注文件已存在，不覆盖人工工作。")
    catalog = catalog_entries(args.catalog)
    rng = random.Random(20261024)
    samples = []
    seen = set()
    for source, part, count in SOURCES:
        report = read_json(PROTOTYPE / "results" / source / "items.json")
        entries = report["items"]
        names = sorted({entry["name"] for entry in catalog if entry["part"] == part})
        ranked = sorted(entries, key=lambda item: item["name_score"])
        # 错字高风险样本 + 随机普通样本；不是按识别成功与否筛样。
        risky = ranked[:max(6, count // 5)]
        pool = [item for item in entries if item not in risky]
        rng.shuffle(pool)
        chosen = []
        for item in risky + pool:
            text = item["name_text"]
            candidates = sorted(names, key=lambda name: SequenceMatcher(
                None, text, name).ratio(), reverse=True)[:3]
            if candidates[0] in seen:
                continue
            seen.add(candidates[0])
            chosen.append((item, candidates))
            if len(chosen) == count:
                break
        rng.shuffle(chosen)
        for index, (item, candidates) in enumerate(chosen):
            samples.append({"id": f"{part}:{item['id']}", "source": source,
                            "crop": f"results/{source}/{item['name_crop']}",
                            "part": part, "ownership": item["ownership"],
                            "column": item["column"], "ocr_text": item["name_text"],
                            "ocr_score": item["name_score"], "suggestions": candidates,
                            "split": "calibration" if index < count // 5 else "evaluation",
                            "text": None, "reviewed": False})
    args.samples.parent.mkdir(parents=True, exist_ok=True)
    write_json(args.samples, {"seed": 20261024, "label_source": "待人工逐图核对；候选不是标签",
                              "samples": samples})
    sheets(args.samples, args.output)


def sheets(samples_path, output):
    samples = read_json(samples_path)["samples"]
    output.mkdir(parents=True, exist_ok=True)
    font = ImageFont.truetype("C:/Windows/Fonts/msyh.ttc", 16)
    for start in range(0, len(samples), 40):
        canvas = Image.new("RGB", (1500, 1800), "white")
        draw = ImageDraw.Draw(canvas)
        for offset, sample in enumerate(samples[start:start + 40]):
            x, y = (offset % 2) * 750, (offset // 2) * 90
            crop = Image.open(PROTOTYPE / sample["crop"]).convert("RGB")
            canvas.paste(crop.resize((432, 80), Image.Resampling.NEAREST), (x, y))
            title = f"{start+offset+1:03} {sample['split'][:1]} {sample['ocr_score']:.2f}"
            draw.text((x+440, y), title, font=font, fill="black")
            draw.text((x+440, y+23), sample.get("text") or sample["suggestions"][0],
                      font=font, fill="black")
            draw.text((x+440, y+47), "OCR: " + sample["ocr_text"], font=font, fill="#555555")
        canvas.save(output / f"labels_{start//40+1}.png")


def calibrate(samples, images, font_path):
    indices = [i for i, sample in enumerate(samples) if sample["split"] == "calibration"]
    queries = gray_features([ink_image(images[i]) for i in indices])
    rows = []
    for size in (12, 13, 14, 15):
        for scale in (1, 4):
            for x in (0, 0.5, 1, 1.5, 2):
                for y in (0, 1, 2, 3, 4, 5):
                    raster = Raster(size, scale, x, y)
                    templates = gray_features([ink_image(render(samples[i]["text"], font_path, raster))
                                               for i in indices])
                    similarities = np.sum(queries * templates, axis=1)
                    by_column = {str(column): float(np.mean([
                        value for i,value in zip(indices,similarities) if samples[i]['column']==column]))
                        for column in sorted({samples[i]['column'] for i in indices})}
                    rows.append({"raster": asdict(raster), "mean_cosine": float(similarities.mean()),
                                 "mean_cosine_by_column": by_column})
    rows.sort(key=lambda row: row["mean_cosine"], reverse=True)
    # UI 的列坐标含不同像素相位；每列用自己的标定图，不逐图调参。
    rasters = {column: Raster(**max(rows,key=lambda row: row['mean_cosine_by_column'][column])['raster'])
               for column in rows[0]['mean_cosine_by_column']}
    return rasters, rows


def edit_distance(a, b):
    previous = list(range(len(b)+1))
    for i, ca in enumerate(a, 1):
        current = [i]
        for j, cb in enumerate(b, 1):
            current.append(min(current[-1]+1, previous[j]+1, previous[j-1]+(ca != cb)))
        previous = current
    return previous[-1]


def choose_threshold(samples, predictions):
    cal = [(sample, prediction) for sample, prediction in zip(samples, predictions)
           if sample["split"] == "calibration"]
    # 阈值只看标定集：排除所有已观察到的错误，选择最宽松的确定性阈值。
    errors = [prediction["margin"] for sample, prediction in cal
              if prediction["text"] != sample["text"]]
    return float(np.nextafter(max(errors), np.inf)) if errors else 1e-6


def metrics(samples, predictions, threshold, split):
    pairs = [(sample, prediction) for sample, prediction in zip(samples, predictions)
             if sample["split"] == split]
    accepted = [(s,p) for s,p in pairs if p["margin"] >= threshold]
    correct = sum(s["text"] == p["text"] for s,p in pairs)
    return {"count": len(pairs), "exact_correct": correct,
            "exact_accuracy": correct / len(pairs),
            "character_error_rate": sum(edit_distance(s["text"], p["text"]) for s,p in pairs)
                                    / sum(len(s["text"]) for s,p in pairs),
            "top5_recall": sum(s["text"] in p["top5"] for s,p in pairs) / len(pairs),
            "accepted": len(accepted), "accepted_wrong": sum(s["text"] != p["text"] for s,p in accepted),
            "coverage": len(accepted) / len(pairs), "margin_threshold": threshold}


def character_stage(samples, images, characters, rasters, font_path, output):
    """隔离字格定位误差：使用经过联系表核对的显式字格框，不冒充自动切字。"""
    records, crops = [], []
    for sample,image in zip(samples,images):
        boxes = sample.get("character_boxes")
        if not boxes:
            continue
        if len(boxes) != len(sample["text"]):
            raise ValueError("字格框数量与标签长度不一致。")
        for index,(text,box) in enumerate(zip(sample["text"],boxes)):
            records.append({"id": f"{sample['id']}:{index}", "text": text,
                            "split": sample["split"], "box": box, "column": sample['column']})
            crops.append(image.crop(box))
    if not records:
        raise ValueError("第一阶段需要样本中显式 character_boxes，不用预测文字代替真实标签。")
    canvas = crops[0].size
    if any(crop.size != canvas for crop in crops):
        raise ValueError("第一阶段的字格必须同尺寸。")
    started = perf_counter()
    origins = {str(sample['column']): sample['character_boxes'][0][0]
               for sample in samples if sample.get('character_boxes')}
    banks = {column: TemplateBank(characters,font_path,
             Raster(raster.size,raster.scale,raster.x-origins[column],raster.y),
             canvas=canvas,binary_size=(16,16)) for column,raster in rasters.items() if column in origins}
    data = {"scope": "30条纯汉字名称的显式字格；不是自动切字或全字符覆盖准确率",
            "banks": {column: bank.description() for column,bank in banks.items()},
            "build_seconds": perf_counter()-started,
            "names": sum(bool(s.get('character_boxes')) for s in samples), "methods": {}}
    predictions_by_method = {}
    for method in ("gray","binary","hybrid"):
        started = perf_counter()
        predictions = [banks[str(record['column'])].search(crop,method) for record,crop in zip(records,crops)]
        elapsed = perf_counter()-started
        predictions_by_method[method] = predictions
        threshold = choose_threshold(records,predictions)
        data["methods"][method] = {"seconds": elapsed,
            "calibration": metrics(records,predictions,threshold,"calibration"),
            "evaluation": metrics(records,predictions,threshold,"evaluation")}
    data["predictions"] = [{**record,"predictions": {m: p[i] for m,p in predictions_by_method.items()}}
                           for i,record in enumerate(records)]
    # 框图保留原图和显式分界，供检查本阶段前提是否成立。
    selected = [(s,image) for s,image in zip(samples,images) if s.get("character_boxes")]
    sheet = Image.new("RGB",(1000, len(selected)*55),"white")
    draw = ImageDraw.Draw(sheet)
    ui = ImageFont.truetype("C:/Windows/Fonts/msyh.ttc",16)
    for row,(sample,image) in enumerate(selected):
        tile = image.resize((324,60),Image.Resampling.NEAREST)
        td = ImageDraw.Draw(tile)
        for x1,y1,x2,y2 in sample["character_boxes"]:
            td.rectangle((x1*3,y1*3,x2*3-1,y2*3-1),outline="#1682c4",width=1)
        sheet.paste(tile,(0,row*55))
        draw.text((340,row*55+12),sample["text"]+" / "+sample["split"],font=ui,fill="black")
    sheet.save(output/"character_boxes.png")
    return data


def fuzzy_predictions(samples, entries):
    pools = {part: sorted({entry["name"] for entry in entries if entry["part"]==part})
             for part in {sample["part"] for sample in samples}}
    predictions = []
    for sample in samples:
        text = sample["ocr_text"]
        scores = [(edit_distance(text,name)/max(len(text),len(name),1),name)
                  for name in pools[sample["part"]]]
        scores.sort()
        predictions.append({"text": scores[0][1], "margin": scores[1][0]-scores[0][0],
                            "score": 1-scores[0][0], "top5": [name for _,name in scores[:5]]})
    return predictions


def reuse_paddle_baseline(samples, previous):
    signature = [(s["id"],s["crop"]) for s in samples]
    if signature != [(s["id"],s["crop"]) for s in previous["predictions"]]:
        raise ValueError("PaddleOCR 基线输入及顺序不一致，不能复用。")
    predictions = [s["predictions"]["paddle_current"] for s in previous["predictions"]]
    measured = dict(previous["methods"]["paddle_current"])
    # 推理输入未变时可复用预测与计时，但标签修订后必须重算准确率。
    measured['evaluation'] = metrics(samples,predictions,0.9,'evaluation')
    return measured,predictions


def run(args):
    args.output.mkdir(parents=True, exist_ok=True)
    samples = load_samples(args.samples)
    catalog = catalog_entries(args.catalog)
    characters, font_info = font_characters(args.font)
    images = [Image.open(PROTOTYPE / sample["crop"]).convert("RGB") for sample in samples]
    if any(image.size != (108,20) for image in images):
        raise ValueError("本实验限定已有108×20名称裁图。")
    cv2.setNumThreads(1)
    started = perf_counter()
    rasters, trials = calibrate(samples, images, args.font)
    result = {"sample_file": str(args.samples), "font": str(args.font),
              "catalog": str(args.catalog), "font_info": font_info,
              "samples": dict(Counter(s["split"] for s in samples)),
              "calibration_seconds": perf_counter()-started, "calibration": trials,
              "raster_by_column": {column: asdict(raster) for column,raster in rasters.items()},
              "methods": {},
              "label_source": read_json(args.samples)["label_source"],
              "label_changes": read_json(args.samples).get('label_changes',[]),
              "environment": {"python": sys.version, "numpy": np.__version__,
                              "opencv": cv2.__version__, "retrieval_threads": 1,
                              "paddle_threads": 4},
              "scope": "已有名称库、已知分类的闭集实验；不代表缺库名称的拒识性能"}
    result["character_stage"] = character_stage(samples,images,characters,rasters,args.font,args.output)
    parts = sorted({sample["part"] for sample in samples})
    banks = {}
    started = perf_counter()
    for part in parts:
        names = sorted({entry["name"] for entry in catalog if entry["part"] == part})
        for column,raster in rasters.items():
            banks[(part,column)] = TemplateBank(names, args.font, raster)
    result["template_build_seconds"] = perf_counter()-started
    result["banks"] = {f'{part}:c{column}': bank.description() for (part,column),bank in banks.items()}
    result["catalog_coverage"] = sum(s["text"] in banks[(s["part"],str(s['column']))].texts for s in samples) / len(samples)
    result["unsupported_catalog_characters"] = ''.join(sorted(
        set(''.join(entry['name'] for entry in catalog))-set(characters)-set(font_info['mapped_whitespace'])))
    all_predictions = {}
    for method in ("gray", "binary", "hybrid"):
        # 一次预热；计时包含局部背景消除、特征提取、全库检索和精排。
        banks[(samples[0]["part"],str(samples[0]['column']))].search(images[0], method)
        durations = []
        for repeat in range(3):
            started = perf_counter()
            predictions = [banks[(sample["part"],str(sample['column']))].search(image, method)
                           for sample,image in zip(samples,images)]
            durations.append(perf_counter()-started)
        threshold = choose_threshold(samples, predictions)
        result["methods"][method] = {
            "seconds_runs": durations, "seconds_per_120": float(np.median(durations))*120/len(samples),
            "calibration": metrics(samples,predictions,threshold,"calibration"),
            "evaluation": metrics(samples,predictions,threshold,"evaluation")}
        all_predictions[method] = predictions
    # 已有 OCR 原文只用于准确率；速度基线另行运行，不能混用历史计时。
    old = [{"text": sample["ocr_text"], "margin": sample["ocr_score"],
            "top5": [sample["ocr_text"]]} for sample in samples]
    result["methods"]["paddle_saved"] = {
        "evaluation": metrics(samples,old,0.9,"evaluation"),
        "note": "历史扫描原文；top5仅有一个结果，margin字段为原OCR分数；不报告历史速度"}
    all_predictions["paddle_saved"] = old
    if args.paddle_report:
        previous = read_json(args.paddle_report)
        measured,predictions = reuse_paddle_baseline(samples,previous)
        result["methods"]["paddle_current"] = measured
        result["paddle_baseline_source"] = str(args.paddle_report)
        all_predictions["paddle_current"] = predictions
    fuzzy_source = all_predictions.get('paddle_current',old)
    fuzzy_samples = [{**sample,'ocr_text': prediction['text']} for sample,prediction in zip(samples,fuzzy_source)]
    started = perf_counter()
    fuzzy = fuzzy_predictions(fuzzy_samples,catalog)
    elapsed = perf_counter()-started
    threshold = choose_threshold(samples,fuzzy)
    result["methods"]["paddle_dictionary"] = {
        "matching_seconds": elapsed, "matching_seconds_per_120": elapsed*120/len(samples),
        "evaluation": metrics(samples,fuzzy,threshold,"evaluation"),
        "note": "同一分类名称库；归一化编辑距离检索。耗时为匹配部分，需另加OCR；候选不强制接受"}
    all_predictions["paddle_dictionary"] = fuzzy
    result["predictions"] = [{**sample, "predictions": {key: values[i] for key,values in all_predictions.items()}}
                             for i,sample in enumerate(samples)]
    write_json(args.output / "report.json", result)
    html_report(result, args.output)
    summary_report(result,args.output)
    print(json.dumps({"rasters": result['raster_by_column'], "methods": result["methods"]},ensure_ascii=False,indent=2))


def html_report(result, output):
    rows = []
    for sample in result["predictions"]:
        crop = os.path.relpath(PROTOTYPE/sample["crop"],output).replace("\\","/")
        columns = []
        for key,prediction in sample["predictions"].items():
            correct = prediction["text"] == sample["text"]
            columns.append(f'<td class="{"ok" if correct else "bad"}">{escape(prediction["text"])}</td>')
        rows.append(f'<tr><td>{escape(sample["id"])}<br>{sample["split"]}</td>'
                    f'<td><img src="{escape(crop)}"></td><td>{escape(sample["text"])}</td>'
                    + "".join(columns) + "</tr>")
    headings = "".join(f"<th>{escape(key)}</th>" for key in result["methods"])
    html = ('<!doctype html><meta charset="utf-8"><title>字体检索离线对照</title>'
            '<style>body{font:16px system-ui;margin:24px}table{border-collapse:collapse}td,th{padding:8px;border-bottom:1px solid #ddd}'
            'img{width:324px;image-rendering:pixelated}.bad{color:#b42318;background:#fff1ed}.ok{color:#067647}</style>'
            '<h1>字体检索离线对照</h1><p>40条标定渲染参数和阈值，160条留出评估。标签由助手视觉核对，尚非用户确认。原图放大3倍。</p>'
            '<table><thead><tr><th>样本</th><th>真实裁图</th><th>视觉核对标签</th>'+headings+'</tr></thead><tbody>'
            + ''.join(rows)+'</tbody></table>')
    (output / "review.html").write_text(html,encoding="utf-8")


def summary_report(result,output):
    lines = ["# 字体检索离线对照", "", result["scope"], "",
             f"标签来源：{result['label_source']}", "",
             f"标定/评估：{result['samples']}。名称库覆盖率：{result['catalog_coverage']:.1%}。",
             "", "## 整条名称", "",
             "| 方法 | 评估正确/总数 | 完全正确率 | 接受数 / 其中错名 | 每120条秒 |",
             "| --- | --- | --- | --- | --- |"]
    for name,method in result["methods"].items():
        m=method['evaluation']; timing=method.get('seconds_per_120')
        if name=='paddle_dictionary':
            timing=method['matching_seconds_per_120']
            baseline=result['methods'].get('paddle_current',{}).get('seconds_per_120')
            timing=f"{baseline+timing:.3f}（OCR+匹配）" if baseline else f"另加匹配{timing:.3f}"
        else:
            timing=f"{timing:.3f}" if timing is not None else '—'
        lines.append(f"| {name} | {m['exact_correct']}/{m['count']} | {m['exact_accuracy']:.2%} | "
                     f"{m['accepted']} / {m['accepted_wrong']} | {timing} |")
    lines += ["", "检索计时包含背景处理、特征提取及精排，不含模板构建和图片读取；取3次中位数。",
              "PaddleOCR为同一批图片上的一次预热后测量；加载时间单列。已有基线不重复运行。",
              "阈值仅由40条标定样本决定，评估集不参与调阈值。尚未验证缺库拒识和ID消歧。", "",
              "## 显式字格的字符检索", "", "| 方法 | 字符正确率 | 正确字符进入前5 |",
              "| --- | --- | --- |"]
    for name,method in result['character_stage']['methods'].items():
        m=method['evaluation'];lines.append(f"| {name} | {m['exact_accuracy']:.2%} | {m['top5_recall']:.2%} |")
    lines += ["", "字格来自30条短纯汉字名称，已保留框图；该阶段不含自动切字，也不是全字库覆盖测试。",
              "", "详见 report.json、review.html、character_boxes.png。"]
    (output/'SUMMARY.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare","sheets","run"))
    parser.add_argument("--font",type=Path,default=PROJECT/"temp/FZYASHJW_Zhong.ttf")
    parser.add_argument("--catalog",type=Path,default=PROJECT/"clothes_all.json")
    parser.add_argument("--samples",type=Path,default=HERE/"samples.json")
    parser.add_argument("--output",type=Path,default=PROTOTYPE/"results/font_retrieval_benchmark")
    parser.add_argument("--paddle-report",type=Path,help="复用未改动的同一批裁图上已测得的PaddleOCR基线，不重复推理")
    args = parser.parse_args()
    if args.command == "prepare":
        prepare(args)
    elif args.command == "sheets":
        sheets(args.samples,args.output)
    else:
        run(args)


if __name__ == "__main__":
    main()

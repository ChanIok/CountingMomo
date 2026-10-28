"""只采样最右侧星星的共同前景，用蒙层造成的亮度差判断拥有状态。"""

from time import perf_counter

import numpy as np
from PIL import Image


def star_mask(config):
    rows = config["foreground_mask"]
    if not rows or not rows[0] or any(len(row) != len(rows[0]) or set(row) - {"0", "1"} for row in rows):
        raise ValueError("星形前景掩码必须为等宽的 0/1 字符串行。")
    mask = np.array([[pixel == "1" for pixel in row] for row in rows])
    if not mask.any():
        raise ValueError("星形前景掩码不能全部为空。")
    return mask


def sample_star(source, config):
    """返回共同前景的 N×3 RGB；分类和实时就绪检查使用同一组像素。"""
    mask = star_mask(config)
    size = (mask.shape[1], mask.shape[0])
    source = source.convert("RGB")
    if source.size != size:
        source = source.resize(size, Image.Resampling.BILINEAR)
    return np.asarray(source, dtype=np.float32)[mask]


def detect_ownership(records, samples, config):
    started = perf_counter()
    states = ("owned", "unowned")
    references = np.asarray([config[f"{state}_luma"] for state in states], dtype=float)
    limit = float(config["max_luma_error"])
    for item, source in zip(records, samples, strict=True):
        pixels = sample_star(source, config)
        # 星芒含抗锯齿像素，因此对共同前景取均值，而非整块矩形或单个亮点。
        luma = float((pixels @ np.array([0.299, 0.587, 0.114])).mean())
        distances = np.abs(references - luma)
        matches = np.flatnonzero(distances <= limit)
        # 参考范围不重叠；落在两组之间或范围之外时留给人工核对。
        state = states[int(matches[0])] if len(matches) == 1 else "unknown"
        item.update(
            star_luma=round(luma, 3),
            star_pixel_count=len(pixels),
            star_owned_error=float(distances[0]),
            star_unowned_error=float(distances[1]),
            ownership=state,
            ownership_basis={
                "owned": "最右侧星星前景匹配已拥有亮度",
                "unowned": "最右侧星星前景匹配未拥有亮度",
                "unknown": "星星前景亮度未匹配参考范围",
            }[state],
        )
    return {"ownership_seconds": perf_counter() - started}

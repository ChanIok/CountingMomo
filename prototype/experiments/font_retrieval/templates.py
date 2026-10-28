"""离线字体实验：统一成像、二进制候选检索和灰度精排；不依赖扫描器。"""

from dataclasses import asdict, dataclass
from pathlib import Path
import struct
import unicodedata

import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont


@dataclass(frozen=True)
class Raster:
    size: int = 13
    scale: int = 4
    x: float = 0
    y: float = 0


def font_characters(path):
    """读取本实验 TrueType 字体的 Unicode cmap，不安装额外字体库。"""
    data = Path(path).read_bytes()
    if data[:4] != b"\x00\x01\x00\x00":
        raise ValueError("本实验需要单个 TrueType 字体，而不是字体合集。")
    u16 = lambda offset: struct.unpack_from(">H", data, offset)[0]
    u32 = lambda offset: struct.unpack_from(">I", data, offset)[0]
    tables = {}
    for index in range(u16(4)):
        tag, _, offset, length = struct.unpack_from(">4sIII", data, 12 + 16 * index)
        tables[tag] = (offset, length)
    cmap = tables[b"cmap"][0]
    mappings = {}
    for index in range(u16(cmap + 2)):
        platform, encoding, relative = struct.unpack_from(">HHI", data, cmap + 4 + 8 * index)
        if platform != 0 and not (platform == 3 and encoding in (1, 10)):
            continue
        offset = cmap + relative
        fmt = u16(offset)
        if fmt == 12:
            for group in range(u32(offset + 12)):
                start, end, glyph = struct.unpack_from(">III", data, offset + 16 + 12 * group)
                mappings.update({cp: glyph + cp - start for cp in range(start, end + 1)
                                 if glyph + cp - start})
        elif fmt == 4:
            count = u16(offset + 6) // 2
            ends = offset + 14
            starts = ends + count * 2 + 2
            deltas = starts + count * 2
            ranges = deltas + count * 2
            for segment in range(count):
                start, end = u16(starts + 2 * segment), u16(ends + 2 * segment)
                delta, relative = u16(deltas + 2 * segment), u16(ranges + 2 * segment)
                for cp in range(start, min(end, 65534) + 1):
                    glyph = ((cp + delta) & 65535) if not relative else u16(
                        ranges + segment * 2 + relative + (cp - start) * 2)
                    if relative and glyph:
                        glyph = (glyph + delta) & 65535
                    if glyph:
                        mappings[cp] = glyph
    if not mappings:
        raise ValueError("字体没有可读取的 Unicode format 4/12 字符映射。")
    characters = [chr(cp) for cp in sorted(mappings)
                  if chr(cp).isprintable() and not chr(cp).isspace()
                  and unicodedata.category(chr(cp)) != "Co"]
    return characters, {"glyphs": u16(tables[b"maxp"][0] + 4),
                        "unicode_characters": len(mappings),
                        "text_characters": len(characters),
                        "mapped_whitespace": [chr(cp) for cp in sorted(mappings)
                                              if chr(cp).isspace()]}


def ink_image(source, percentile=95):
    """局部闭运算估计浅色背景，保留暗色笔画；不把拥有状态作为识别条件。"""
    gray = np.asarray(source.convert("L")) if isinstance(source, Image.Image) else source
    gray = np.asarray(gray, dtype=np.uint8)
    ink = cv2.morphologyEx(gray, cv2.MORPH_BLACKHAT, np.ones((5, 5), np.uint8))
    level = max(1.0, float(np.percentile(ink, percentile)))
    return np.clip(ink.astype(np.float32) / level, 0, 1)


def render(text, font_path, raster, canvas=(108, 20), font=None):
    scale = raster.scale
    font = font or ImageFont.truetype(str(font_path), raster.size * scale)
    image = Image.new("L", (canvas[0] * scale, canvas[1] * scale), 255)
    ImageDraw.Draw(image).text((raster.x * scale, raster.y * scale), text, font=font, fill=0)
    if scale != 1:
        image = image.resize(canvas, Image.Resampling.LANCZOS)
    return image


def gray_features(images):
    matrix = np.asarray(images, dtype=np.float32).reshape(len(images), -1)
    return matrix / np.maximum(np.linalg.norm(matrix, axis=1, keepdims=True), 1e-8)


def binary_features(images, size):
    resized = np.stack([cv2.resize(image, size, interpolation=cv2.INTER_AREA)
                        for image in images])
    return np.packbits((resized >= 0.35).reshape(len(images), -1), axis=1)


def hamming_distances(query, bank):
    return np.bitwise_count(np.bitwise_xor(bank, query)).sum(axis=1, dtype=np.int32)


def best_indices(scores, count, largest=False):
    """稳定排序使同分候选可复现；不隐含把同分视作确定答案。"""
    return np.argsort(-scores if largest else scores, kind="stable")[:count]


class TemplateBank:
    def __init__(self, texts, font_path, raster, canvas=(108, 20), binary_size=(64, 16), ink_percentile=95):
        self.texts = list(texts)
        self.raster = raster
        self.canvas = canvas
        self.binary_size = binary_size
        self.ink_percentile = ink_percentile
        font = ImageFont.truetype(str(font_path), raster.size * raster.scale)
        images = [ink_image(render(text, font_path, raster, canvas, font), ink_percentile) for text in texts]
        self.gray = gray_features(images)
        self.binary = binary_features(images, binary_size)

    def search(self, image, method, top_k=20):
        ink = ink_image(image, self.ink_percentile)
        query = gray_features([ink])[0]
        if method == "gray":
            similarity = self.gray @ query
            indices = best_indices(similarity, 5, largest=True)
            margin = float(similarity[indices[0]] - similarity[indices[1]])
        elif method in ("binary", "hybrid"):
            distances = hamming_distances(binary_features([ink], self.binary_size)[0], self.binary)
            candidates = best_indices(distances, max(5, top_k))
            if method == "binary":
                indices = candidates[:5]
                similarity = 1 - distances / (self.binary_size[0] * self.binary_size[1])
                margin = float(similarity[indices[0]] - similarity[indices[1]])
            else:
                scores = self.gray[candidates] @ query
                order = best_indices(scores, 5, largest=True)
                indices = candidates[order]
                similarity = {int(i): float(score) for i, score in zip(candidates, scores)}
                margin = float(similarity[int(indices[0])] - similarity[int(indices[1])])
        else:
            raise ValueError(f"未知检索方法：{method}")
        return {"text": self.texts[indices[0]], "margin": margin,
                "score": float(similarity[int(indices[0])]),
                "top5": [self.texts[i] for i in indices]}

    def description(self):
        return {"templates": len(self.texts), "raster": asdict(self.raster),
                "canvas": self.canvas, "binary_size": self.binary_size,
                "ink_percentile": self.ink_percentile,
                "binary_bytes": self.binary.nbytes, "gray_bytes": self.gray.nbytes}

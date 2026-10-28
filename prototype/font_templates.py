"""游戏字体的字符映射与灰度模板；运行时不读取服装名称库。"""

from dataclasses import dataclass, field
from pathlib import Path
import struct
import unicodedata

import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont


EXCLUDED_CHARACTERS = frozenset('•')
LINE_HEIGHT = 20
BASELINE_Y = 3


def font_characters(path):
    data = Path(path).read_bytes()
    if data[:4] != b'\x00\x01\x00\x00':
        raise ValueError('名称识别需要单个TrueType字体文件。')
    u16 = lambda offset: struct.unpack_from('>H', data, offset)[0]
    u32 = lambda offset: struct.unpack_from('>I', data, offset)[0]
    tables = {}
    for index in range(u16(4)):
        tag, _, offset, length = struct.unpack_from('>4sIII', data, 12 + 16 * index)
        if offset + length > len(data):
            raise ValueError('字体表超出文件范围。')
        tables[tag] = offset
    cmap, mappings = tables[b'cmap'], {}
    for index in range(u16(cmap + 2)):
        platform, encoding, relative = struct.unpack_from('>HHI', data, cmap + 4 + 8 * index)
        if platform != 0 and not (platform == 3 and encoding in (1, 10)):
            continue
        offset = cmap + relative
        if u16(offset) == 12:
            for group in range(u32(offset + 12)):
                start, end, glyph = struct.unpack_from('>III', data, offset + 16 + 12 * group)
                mappings.update({cp: glyph + cp - start for cp in range(start, end + 1)
                                 if glyph + cp - start})
        elif u16(offset) == 4:
            count = u16(offset + 6) // 2
            ends = offset + 14
            starts = ends + count * 2 + 2
            deltas, ranges = starts + count * 2, starts + count * 4
            for segment in range(count):
                start, end = u16(starts + 2 * segment), u16(ends + 2 * segment)
                delta, relative = u16(deltas + 2 * segment), u16(ranges + 2 * segment)
                for cp in range(start, min(end, 65534) + 1):
                    glyph = ((cp + delta) & 65535) if not relative else u16(
                        ranges + 2 * segment + relative + (cp - start) * 2)
                    if relative and glyph:
                        glyph = (glyph + delta) & 65535
                    if glyph:
                        mappings[cp] = glyph
    characters = tuple(chr(cp) for cp in sorted(mappings)
                       if chr(cp).isprintable() and not chr(cp).isspace()
                       and unicodedata.category(chr(cp)) != 'Co'
                       and chr(cp) not in EXCLUDED_CHARACTERS)
    if len(characters) < 2:
        raise ValueError('字体没有足够的可用Unicode字符。')
    return characters, {'glyphs': u16(tables[b'maxp'] + 4),
                        'unicode_characters': len(mappings), 'candidate_characters': len(characters),
                        'excluded_characters': sorted(EXCLUDED_CHARACTERS)}


def ink_pixels(image):
    gray = np.asarray(image.convert('L')) if isinstance(image, Image.Image) else image
    return cv2.morphologyEx(gray, cv2.MORPH_BLACKHAT, np.ones((5, 5), np.uint8))


def gray_vector(ink):
    values = ink.astype(np.float32)
    values = np.minimum(values, max(1., float(np.percentile(values, 99)))).ravel()
    return values / max(float(np.linalg.norm(values)), 1e-8)


@dataclass
class FontTemplates:
    path: Path
    characters: tuple
    info: dict
    banks: dict = field(default_factory=dict)
    widths: dict = field(default_factory=dict)


def template_bank(templates, size):
    if size not in templates.banks:
        font = ImageFont.truetype(str(templates.path), size)
        vectors = []
        for char in templates.characters:
            image = Image.new('L', (size + 1, LINE_HEIGHT), 255)
            ImageDraw.Draw(image).text((.5, BASELINE_Y), char, font=font, fill=0)
            vectors.append(gray_vector(ink_pixels(image)))
        templates.banks[size] = np.stack(vectors)
        templates.widths[size] = tuple(font.getlength(char) for char in templates.characters)
    return templates.banks[size]


def similarities(queries, bank):
    # OpenCV的矩阵乘法避免逐字符启动BLAS线程；模板和输入均为归一化float32。
    return cv2.gemm(np.stack(queries), bank, 1., None, 0., flags=cv2.GEMM_2_T)

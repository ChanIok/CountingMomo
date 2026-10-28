"""星形前景判定及背景干扰用例；不加载模型或操作窗口。"""

import json
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

from ownership import detect_ownership, sample_star, star_mask


class OwnershipTests(unittest.TestCase):
    def setUp(self):
        root = Path(__file__).resolve().parents[1]
        self.config = json.loads((root / "layouts/portrait_1_4.json").read_text(encoding="utf-8"))["ownership_star"]
        self.mask = star_mask(self.config)

    def sample(self, foreground, background):
        pixels = np.full((*self.mask.shape, 3), background, dtype=np.uint8)
        pixels[self.mask] = foreground
        return Image.fromarray(pixels)

    def classify(self, source):
        records = [{}]
        detect_ownership(records, [source], self.config)
        return records[0]

    def test_samples_all_21_foreground_pixels(self):
        pixels = sample_star(self.sample(201, 0), self.config)
        self.assertEqual(pixels.shape, (21, 3))
        self.assertTrue(np.all(pixels == 201))

    def test_background_does_not_change_state_or_luma(self):
        for foreground, state in ((201, "owned"), (162, "unowned")):
            dark = self.classify(self.sample(foreground, 0))
            white = self.classify(self.sample(foreground, 255))
            self.assertEqual(dark["ownership"], state)
            self.assertEqual(white["ownership"], state)
            self.assertEqual(dark["star_luma"], white["star_luma"])

    def test_between_or_outside_reference_ranges_is_unknown(self):
        for luma in (0, 180, 255):
            self.assertEqual(self.classify(self.sample(luma, 0))["ownership"], "unknown")


if __name__ == "__main__":
    unittest.main()

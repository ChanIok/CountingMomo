"""模拟帧与可选历史截图回放；不导入 WGC/Win32，不加载 OCR。"""

import json
import random
import unittest
from pathlib import Path
from types import SimpleNamespace

import cv2
import numpy as np
from PIL import Image, ImageDraw

from stability import PageWaitError, wait_page
from tracking import (PageNotReady, inputs_stable, locate_anchor, make_anchor,
                      read_page, reference_image, row_advance)


CONFIG = {
    "reference_size": [360, 960], "columns": [18, 132, 246],
    "first_row_y": 60, "row_step": 60, "card_size": [106, 48],
    "card_frame": {"bottom_rect": [4, 39, 22, 47], "right_rect": [20, 12, 29, 38],
                   "min_contrast": 20},
    "safe_y": [60, 850], "scrolled_safe_top": 150,
    "name_rect": [28, 4, 100, 24], "name_tracking_rect": [28, 4, 100, 24],
    "name_icon_rect": [100, 0, 106, 20],
    "star_rect": [4, 4, 12, 12],
    "ownership_star": {"foreground_mask": ["11111111"] * 8,
                       "owned_luma": 200, "unowned_luma": 160, "max_luma_error": 10},
    "bottom_detection": {
        "line_rect": [349, 900, 352, 930], "left_rect": [344, 900, 347, 930],
        "right_rect": [355, 900, 358, 930], "min_contrast": 25,
        "min_clear_fraction": 0.9, "safe_bottom": 910,
    },
}


def render(displacement=0, count=150, bottom=False, textured=False):
    image = Image.new("RGB", (360, 960), (170, 150, 120))
    if textured:
        y, x = np.indices((960, 360))
        texture = (x % 13 - 6) * 1.4 + (y % 17 - 8) * 0.6
        image = Image.fromarray((np.asarray(image) + texture[:, :, None]).clip(0, 255).astype("uint8"))
    draw = ImageDraw.Draw(image)
    for item in range(count):
        row, column = divmod(item, 3)
        x, y = CONFIG["columns"][column], 60 + row * 60 - displacement
        if y + 48 < 0 or y >= 960:
            continue
        draw.rectangle((x, y, x + 105, y + 47), fill=(200, 180, 150), outline="white")
        draw.rectangle((x + 2, y + 2, x + 24, y + 43), fill=(74, 55, 44), outline="white")
        rng = random.Random(item)
        text = "".join(rng.choice("ABCDEFGHIJKLMNOPQRSTUVWXYZ") for _ in range(8))
        draw.text((x + 28, y + 4), text, fill=(45, 35, 25))
    if bottom:
        draw.rectangle((349, 900, 351, 929), fill="white")
    return image


class Clock:
    def __init__(self):
        self.time = 0.0

    def now(self):
        return self.time

    def pause(self, seconds):
        self.time += seconds


class Frames:
    def __init__(self, clock, schedule, duplicate_source=False):
        self.clock = clock
        self.frames = [(when, SimpleNamespace(
            sequence=index + 1, received_at=when,
            source_time=1 if duplicate_source else index + 1,
            pixels=cv2.cvtColor(np.asarray(image), cv2.COLOR_RGB2BGRA)))
            for index, (when, image) in enumerate(schedule)]

    def snapshot(self):
        visible = [frame for when, frame in self.frames if when <= self.clock.time]
        return visible[-1] if visible else None


class ScanningTests(unittest.TestCase):
    def setUp(self):
        self.image = render()
        self.previous = read_page(self.image, CONFIG, 60, at_top=True)
        self.anchor = make_anchor(self.previous, CONFIG)

    def wait(self, schedule, timeout=2, duplicate_source=False):
        clock = Clock()
        capture = Frames(clock, schedule, duplicate_source)
        result = wait_page(capture, CONFIG, lambda: None, 0.25, timeout,
                           previous=self.previous, anchor=self.anchor,
                           clock=clock.now, pause=clock.pause)
        return result, clock.time

    def test_name_anchor_measures_motion_without_icons(self):
        match = locate_anchor(reference_image(render(120), CONFIG), self.anchor, CONFIG)
        self.assertTrue(match["valid"])
        self.assertEqual(match["displacement"], 120)

    def test_unchanged_frames_do_not_complete_scroll(self):
        with self.assertRaises(PageWaitError) as caught:
            self.wait([(0, self.image), (0.4, self.image), (0.8, self.image)], timeout=1)
        self.assertIn("等待滚动响应", str(caught.exception))

    def test_delayed_response_is_not_assigned_to_another_input(self):
        shifted = render(120)
        result, elapsed = self.wait([(0, self.image), (0.4, self.image),
                                     (0.8, shifted), (1.0, shifted), (1.2, shifted)])
        self.assertEqual(result.displacement, 120)
        self.assertGreaterEqual(elapsed, 1.2)
        self.assertEqual(result.advance, 4)

    def test_cached_frame_cannot_become_second_stable_sample(self):
        with self.assertRaises(PageWaitError):
            self.wait([(0, render(120))], timeout=1)

    def test_duplicate_source_time_cannot_prove_stability(self):
        with self.assertRaises(PageWaitError):
            self.wait([(0, render(120)), (0.4, render(120)), (0.8, render(120))],
                      timeout=1, duplicate_source=True)

    def test_icons_and_attributes_do_not_affect_readiness(self):
        altered = self.image.copy()
        draw = ImageDraw.Draw(altered)
        for row in self.previous.rows:
            for x in CONFIG["columns"]:
                draw.rectangle((x + 14, row.y + 24, x + 24, row.y + 43), fill="blue")
                draw.rectangle((x + 35, row.y + 30, x + 95, row.y + 40), fill="red")
        page = read_page(altered, CONFIG, 60, at_top=True)
        self.assertTrue(inputs_stable(self.previous, page))
        self.assertTrue(locate_anchor(page.reference, self.anchor, CONFIG)["valid"])

    def test_one_local_star_update_is_not_averaged_away(self):
        altered = self.image.copy()
        x, y = CONFIG["columns"][-1], self.previous.rows[-1].y
        ImageDraw.Draw(altered).rectangle((x + 4, y + 4, x + 11, y + 11), fill="white")
        self.assertFalse(inputs_stable(self.previous, read_page(altered, CONFIG, 60, at_top=True)))

    def test_one_local_name_update_is_not_averaged_away(self):
        altered = self.image.copy()
        x, y = CONFIG["columns"][-1], self.previous.rows[-1].y
        ImageDraw.Draw(altered).rectangle((x + 65, y + 16, x + 72, y + 21), fill="black")
        self.assertFalse(inputs_stable(self.previous, read_page(altered, CONFIG, 60, at_top=True)))

    def test_ambiguous_name_anchor_is_rejected(self):
        altered = self.image.copy()
        for index, templates in enumerate(self.anchor.templates):
            for x, template in zip(CONFIG["columns"], templates, strict=True):
                altered.paste(Image.fromarray(template).convert("RGB"), (x + 28, 180 + index * 60 + 4))
        self.assertFalse(locate_anchor(reference_image(altered, CONFIG), self.anchor, CONFIG)["valid"])

    def test_partial_last_row_and_blank_tail_are_not_exported_as_items(self):
        page = read_page(render(count=32, bottom=True), CONFIG, 60, at_top=True)
        self.assertTrue(page.bottom["at_bottom"])
        self.assertEqual(sum(len(row.cards) for row in page.rows), 32)
        self.assertEqual(len(page.rows[-1].cards), 2)

    def test_empty_slot_before_bottom_is_not_silently_skipped(self):
        with self.assertRaises(PageNotReady):
            read_page(render(count=20), CONFIG, 60, at_top=True)

    def test_textured_empty_tail_is_not_treated_as_loading_cards(self):
        page = read_page(render(count=31, bottom=True, textured=True), CONFIG, 60, at_top=True)
        self.assertEqual(sum(len(row.cards) for row in page.rows), 31)
        self.assertEqual(len(page.rows[-1].cards), 1)

    def test_present_card_with_missing_name_still_waits(self):
        altered = self.image.copy()
        x, y = CONFIG["columns"][-1], self.previous.rows[-1].y
        ImageDraw.Draw(altered).rectangle((x + 28, y + 4, x + 99, y + 23), fill=(200, 180, 150))
        with self.assertRaisesRegex(PageNotReady, "名称尚未就绪"):
            read_page(altered, CONFIG, 60, at_top=True)

    def test_missing_card_in_middle_at_bottom_still_waits(self):
        altered = render(count=32, bottom=True)
        x, y = CONFIG["columns"][1], CONFIG["first_row_y"]
        ImageDraw.Draw(altered).rectangle((x, y, x + 105, y + 47), fill=(170, 150, 120))
        with self.assertRaisesRegex(PageNotReady, "网格中间出现空位"):
            read_page(altered, CONFIG, 60, at_top=True)

    def test_grid_phase_and_global_row_advance_agree(self):
        current = read_page(render(120), CONFIG, self.previous.phase - 120)
        self.assertEqual(row_advance(self.previous, current, 120, CONFIG), 4)


class RecordedRegressionTests(unittest.TestCase):
    def test_hair_820_partial_last_row(self):
        root = Path(__file__).resolve().parents[1]
        archive = root / "results/live_20261024_032029_206165"
        final_frame = archive / "unmatched_frame.png"
        if not final_frame.is_file():
            self.skipTest("本机发型820件失败现场未提供。")
        config = json.loads((root / "layouts/portrait_1_4.json").read_text(encoding="utf-8"))
        with Image.open(final_frame) as source:
            page = read_page(source.convert("RGB"), config, 3703)
        self.assertTrue(page.bottom["at_bottom"])
        self.assertEqual(page.rows[-1].y, 3703)
        self.assertEqual(len(page.rows[-1].cards), 1)
        self.assertEqual(sum(len(row.cards) for row in page.rows), 112)
        session = json.loads((archive / "session.json").read_text(encoding="utf-8"))
        previous_start = session["frames"][-1]["global_start"]
        global_start = previous_start + (page.rows[0].y - 433 + 2186) // config["row_step"]
        unseen = [row for offset, row in enumerate(page.rows) if global_start + offset >= 247]
        self.assertEqual(sum(len(row.cards) for row in unseen), 79)

    def test_late_icon_refresh_keeps_the_same_name_anchor(self):
        root = Path(__file__).resolve().parents[1]
        archive = root / "results/live_20261024_020710_245400"
        before, after = archive / "frames/f0005.jpg", archive / "unmatched_frame.png"
        if not before.is_file() or not after.is_file():
            self.skipTest("本机历史截图未提供；合成帧用例仍可运行。")
        config = json.loads((root / "layouts/portrait_1_4.json").read_text(encoding="utf-8"))
        with Image.open(before) as source:
            page = read_page(source.convert("RGB"), config, 385)
        anchor = make_anchor(page, config)
        with Image.open(after) as source:
            match = locate_anchor(reference_image(source.convert("RGB"), config), anchor, config)
        self.assertTrue(match["valid"])
        self.assertEqual(match["displacement"], 0)


if __name__ == "__main__":
    unittest.main()

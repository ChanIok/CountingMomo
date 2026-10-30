"""纯图像和模拟流程检查；不导入窗口模块、不连接游戏或发送输入。"""

import ctypes
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

import cv2
import numpy as np

import category_navigation as nav


class CategoryNavigationTests(unittest.TestCase):
    def setUp(self):
        self.config = nav.load_catalog()

    def runner(self):
        runner = nav.Navigator.__new__(nav.Navigator)
        runner.config = self.config
        runner.args = SimpleNamespace(timeout=1, max_scrolls=3)
        runner.stage = "模拟事务"
        runner.check = Mock()
        runner.journal = Mock()
        runner.last_frame = None
        runner.pointer_parked = True
        return runner

    def test_catalog_has_unique_targets_and_both_empty_samples(self):
        groups = self.config["groups"]
        self.assertEqual([len(group["categories"]) for group in groups], [8, 15, 7])
        entries = [entry for group in groups for entry in group["categories"]]
        self.assertEqual(len({entry["id"] for entry in entries}), 30)
        self.assertEqual([entry["name"] for entry in entries if entry.get("sample_empty")],
                         ["全妆", "肤色"])
        self.assertTrue(all(np.linalg.norm(entry["signal"]) > 0 for entry in entries))

    def test_notches_are_calculated_and_bounded(self):
        self.assertEqual(nav.scroll_notches(658 - 222, 24, 24), 19)
        self.assertEqual(nav.scroll_notches(55, 24, 12), 3)
        self.assertEqual(nav.scroll_notches(295, 24, 12), 12)

    def test_icon_is_located_after_translation(self):
        entry = self.config["groups"][0]["categories"][1]
        pixels = np.full((750, 300, 3), 170, dtype=np.uint8)
        template = cv2.imread(str(nav.TEMPLATE_ROOT / entry["template"]))
        h, w = template.shape[:2]
        pixels[300:300+h, 200:200+w] = template
        match = nav.locate_icon(pixels, entry, [180, 95, 279, 704], self.config)
        self.assertIsNotNone(match)
        self.assertEqual(match.point, (228, 328))

    def test_absent_and_ambiguous_icons_are_not_clicked(self):
        entry = self.config["groups"][0]["categories"][1]
        pixels = np.full((750, 300, 3), 170, dtype=np.uint8)
        self.assertIsNone(nav.locate_icon(pixels, entry, [180, 95, 279, 704], self.config))
        template = cv2.imread(str(nav.TEMPLATE_ROOT / entry["template"]))
        h, w = template.shape[:2]
        for y in (200, 400):
            pixels[y:y+h, 200:200+w] = template
        self.assertIsNone(nav.locate_icon(pixels, entry, [180, 95, 279, 704], self.config))

    def test_selection_requires_all_four_edges(self):
        pixels = np.full((750, 300, 3), 170, dtype=np.uint8)
        self.assertLess(nav.selection_contrast(pixels, (229, 400), child=True), 3)
        cv2.rectangle(pixels, (197, 368), (261, 432), (240, 240, 240), 1)
        self.assertGreater(nav.selection_contrast(pixels, (229, 400), child=True), 3)
        pixels[425:440, 190:270] = 170
        self.assertLess(nav.selection_contrast(pixels, (229, 400), child=True), 3)

    def wait_with_frames(self, frames, **kwargs):
        runner = self.runner()
        runner.stable_sample = lambda frame: [frame.pixels]
        sequence = iter(frames)
        runner.capture = SimpleNamespace(snapshot=lambda: next(sequence, frames[-1]))
        clock = [0.0]

        def tick():
            clock[0] += 0.05
            return clock[0]

        with patch.object(nav, "monotonic", side_effect=tick), patch.object(nav, "sleep"):
            return runner.wait(motion_box=[0, 0, 2, 2], **kwargs)

    def test_stability_needs_distinct_source_frames(self):
        pixels = np.zeros((2, 2, 3), dtype=np.uint8)
        frames = [SimpleNamespace(sequence=i, source_time=1, received_at=0, pixels=pixels)
                  for i in range(1, 30)]
        with self.assertRaises(TimeoutError):
            self.wait_with_frames(frames)
        for frame in frames:
            frame.source_time = frame.sequence
        self.assertGreater(self.wait_with_frames(frames).sequence, 1)

    def test_unanswered_scroll_times_out(self):
        pixels = np.zeros((2, 2, 3), dtype=np.uint8)
        frames = [SimpleNamespace(sequence=i, source_time=i, received_at=0, pixels=pixels)
                  for i in range(1, 30)]
        with self.assertRaises(TimeoutError):
            self.wait_with_frames(frames[1:], marker=frames[0], moved=True)

    def test_scroll_timeout_does_not_append_another_input(self):
        runner = self.runner()
        runner.input = Mock(return_value=(Mock(), 0))
        runner.wait = Mock(side_effect=TimeoutError("未响应"))
        frame = SimpleNamespace(pixels=np.zeros((750, 300, 3), dtype=np.uint8))
        group = self.config["groups"][0]
        with self.assertRaises(TimeoutError):
            runner.seek(group["categories"][0], group, frame)
        runner.input.assert_called_once()

    def test_wheel_keeps_pointer_inside_the_menu(self):
        # 以下都是内存中的假输入结构与 Mock，不调用 Win32。
        class Mouse(ctypes.Structure):
            _fields_ = [("mouseData", ctypes.c_uint32), ("dwFlags", ctypes.c_uint32)]

        class Input(ctypes.Structure):
            _fields_ = [("type", ctypes.c_uint32), ("mi", Mouse)]

        runner = self.runner()
        runner.hwnd, runner.pid, runner.size = 1, 2, (1440, 5760)
        runner.win = SimpleNamespace(position_pointer=Mock(), Input=Input, MouseInput=Mouse,
                                     user32=SimpleNamespace(SendInput=Mock(return_value=1)))
        runner.capture = SimpleNamespace(snapshot=Mock(return_value=Mock()))
        runner.journal = Mock()
        runner.park_pointer = Mock()
        clock = [0.0]

        def tick():
            clock[0] += 0.1
            return clock[0]

        with patch.object(nav, "monotonic", side_effect=tick), patch.object(nav, "sleep"):
            runner.input((229, 451), wheel=1)
        runner.park_pointer.assert_not_called()
        self.assertFalse(runner.pointer_parked)

    def test_unmatched_visible_child_stops_instead_of_scrolling_past_it(self):
        runner = self.runner()
        group = self.config["groups"][1]
        gloves = next(entry for entry in group["categories"] if entry["id"] == "gloves")
        runner.save_region = Mock()
        runner.journal = Mock()
        with patch.object(nav, "locate_icon", return_value=nav.IconMatch((229, 524), 0.99, 0.5)):
            with self.assertRaisesRegex(RuntimeError, "未继续滚过该分类"):
                runner.reject_missed_child(gloves, group, SimpleNamespace(pixels=Mock()),
                                           [188, 265, 272, 637])
        runner.save_region.assert_called_once()

    def test_collar_category_is_between_bracelets_and_gloves(self):
        runner = self.runner()
        group = self.config["groups"][1]
        entries = group["categories"]
        index = next(i for i, item in enumerate(entries) if item["id"] == "bracelets")
        self.assertEqual([item["id"] for item in entries[index:index+3]],
                         ["bracelets", "collars", "gloves"])
        runner.save_region = Mock()
        runner.journal = Mock()

        def locate(_pixels, entry, *_args, **_kwargs):
            return nav.IconMatch((229, 524), 0.99, 0.5) if entry["id"] == "bracelets" else None

        with patch.object(nav, "locate_icon", side_effect=locate):
            expected = runner.reject_missed_child(entries[index+2], group,
                                                  SimpleNamespace(pixels=Mock()), [188, 265, 272, 637])
        self.assertEqual(expected, (229, 660))
        runner.save_region.assert_not_called()

    def test_missing_parent_stops_before_scrolling(self):
        runner = self.runner()
        runner.input = Mock()
        frame = SimpleNamespace(pixels=np.zeros((750, 300, 3), dtype=np.uint8))
        group = self.config["groups"][1]
        with self.assertRaisesRegex(RuntimeError, "菜单边界不明"):
            runner.seek(group["categories"][0], group, frame)
        runner.input.assert_not_called()

    def test_accessory_bounds_need_the_expanded_gap_and_visible_next_parent(self):
        runner = self.runner()
        group = self.config["groups"][1]
        pixels = Mock()
        with patch.object(nav, "locate_icon", side_effect=[
                nav.IconMatch((229, 658), 0.99, 0.5), None]):
            self.assertIsNone(runner.bounds(pixels, group))
        with patch.object(nav, "locate_icon", side_effect=[
                nav.IconMatch((229, 585), 0.99, 0.5),
                nav.IconMatch((229, 675), 0.99, 0.5)]):
            self.assertIsNone(runner.bounds(pixels, group))
        with patch.object(nav, "locate_icon", side_effect=[
                nav.IconMatch((229, 222), 0.99, 0.5),
                nav.IconMatch((229, 675), 0.99, 0.5)]):
            self.assertEqual(runner.bounds(pixels, group), [188, 265, 272, 637])

    def test_makeup_viewport_must_fit_before_scrolling_inside_it(self):
        runner = self.runner()
        group = self.config["groups"][2]
        with patch.object(nav, "locate_icon", return_value=nav.IconMatch((229, 675), 0.99, 0.5)):
            self.assertIsNone(runner.bounds(Mock(), group))
        with patch.object(nav, "locate_icon", return_value=nav.IconMatch((229, 306), 0.99, 0.5)):
            self.assertEqual(runner.bounds(Mock(), group), [188, 349, 272, 724])

    def test_reveal_uses_top_outer_point_not_the_moving_parent(self):
        runner = self.runner()
        group = self.config["groups"][1]
        frames = [SimpleNamespace(pixels=Mock()) for _ in range(2)]
        runner.bounds = Mock(side_effect=[None, [188, 265, 272, 637]])
        runner.input = Mock(return_value=(Mock(), 0))
        runner.wait = Mock(side_effect=frames[1:])
        runner.journal = Mock()
        with patch.object(nav, "locate_icon", return_value=nav.IconMatch((229, 658), 0.99, 0.5)):
            self.assertIs(runner.reveal_group(group, frames[0]), frames[1])
        self.assertEqual([call.args[0] for call in runner.input.call_args_list],
                         [(229, 126)])
        runner.input.assert_called_once_with((229, 126), wheel=19)

    def test_open_group_does_not_require_children_before_revealing_them(self):
        runner = self.runner()
        group = self.config["groups"][1]
        hidden = SimpleNamespace(pixels=Mock())
        visible = SimpleNamespace(pixels=Mock())
        match = nav.IconMatch((229, 658), 0.99, 0.5)
        runner.seek = Mock(side_effect=[(hidden, match), (visible, match)])
        runner.input = Mock(return_value=(Mock(), 0))
        runner.selected = Mock(return_value=True)

        def confirm_parent(**kwargs):
            self.assertTrue(kwargs["predicate"](hidden.pixels))
            runner.selected.assert_called_once_with(hidden.pixels, group["entry"], group, root=True)
            return hidden

        runner.wait = Mock(side_effect=confirm_parent)
        runner.reveal_group = Mock(return_value=visible)
        runner.bounds = Mock(return_value=[188, 265, 272, 637])
        with patch.object(nav, "locate_icon", return_value=match):
            self.assertIs(runner.open_group(group, hidden), visible)
        runner.reveal_group.assert_called_once_with(group, hidden)

    def test_f8_stops_before_accessing_window_or_sending_input(self):
        runner = self.runner()
        runner.win = SimpleNamespace(stop_pressed=lambda: True)
        with self.assertRaises(nav.NavigationStopped):
            nav.Navigator.check(runner)

    def test_empty_category_is_visited_without_reading_items(self):
        runner = self.runner()
        group = self.config["groups"][2]
        entry = group["categories"][0]
        match = nav.IconMatch((229, 386), 0.99, 0.5)
        frame = SimpleNamespace(pixels=Mock(), sequence=7, source_time=8)
        runner.seek = Mock(return_value=(frame, match))
        runner.selected = Mock(return_value=True)
        runner.bounds = Mock(return_value=[188, 349, 272, 728])
        runner.save_region = Mock()
        runner.journal = Mock()
        runner.input = Mock()
        runner.session = {"planned": [{"id": entry["id"]}], "visited": []}
        with tempfile.TemporaryDirectory() as folder:
            runner.output = Path(folder)
            with patch.object(nav, "locate_icon", return_value=match):
                runner.visit(entry, group, frame)
            self.assertEqual(runner.session["remaining"], [])
            self.assertEqual(runner.session["visited"][0]["name"], "全妆")
            self.assertTrue(runner.session["visited"][0]["sample_empty"])
            runner.input.assert_not_called()
            self.assertTrue((runner.output / "session.json").exists())


if __name__ == "__main__":
    unittest.main()

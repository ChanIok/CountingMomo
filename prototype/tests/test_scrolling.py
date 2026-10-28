"""固定大步标定的纯计算用例，不导入 Win32。"""

import unittest

from scrolling import (MAX_WHEEL_NOTCHES, calibrated_notches,
                       target_displacement, verify_response)


CONFIG = {"scrolled_safe_top": 360, "safe_y": [100, 3612],
          "row_step": 88, "card_size": [217, 74]}


class ScrollingTests(unittest.TestCase):
    def test_target_uses_safe_height_and_reserved_rows(self):
        self.assertEqual(target_displacement(CONFIG, 6), 2601)
        self.assertLess(target_displacement(CONFIG, 10), 2601)

    def test_one_calibration_selects_fixed_page_input(self):
        self.assertEqual(calibrated_notches(96, 2601), (162, 16))

    def test_no_motion_is_not_a_calibration(self):
        with self.assertRaises(RuntimeError):
            calibrated_notches(0, 2601)

    def test_event_delta_stays_within_signed_short(self):
        notches, _ = calibrated_notches(2, 2601)
        self.assertEqual(notches, MAX_WHEEL_NOTCHES)
        self.assertLessEqual(notches * 120, 32767)

    def test_response_mismatch_stops_instead_of_relearning(self):
        with self.assertRaises(RuntimeError):
            verify_response(384, 2592, False, 88)
        verify_response(2592, 2592, False, 88)
        verify_response(200, 2592, True, 88)

    def test_invalid_overlap_is_rejected(self):
        with self.assertRaises(ValueError):
            target_displacement(CONFIG, 1)
        with self.assertRaises(ValueError):
            target_displacement(CONFIG, 100)


if __name__ == "__main__":
    unittest.main()

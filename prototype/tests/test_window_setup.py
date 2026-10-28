"""只模拟 Win32 返回值，不调整真实窗口、不发送输入。"""

import unittest
from unittest.mock import Mock, patch

import window_input


class WindowSetupTests(unittest.TestCase):
    def setUp(self):
        self.now = 0.0
        self.api = Mock()
        self.api.IsIconic.return_value = False
        self.api.IsZoomed.return_value = False
        self.api.GetWindowLongW.return_value = 0x10CF0000
        self.api.GetMonitorInfoW.side_effect = self.monitor_info

    def monitor_info(self, _monitor, pointer):
        pointer._obj.rcMonitor = window_input.wintypes.RECT(0, 0, 2560, 1440)
        return True

    def pause(self, seconds):
        self.now += seconds

    def prepare(self, geometry, timeout=1, check_stop=lambda: None):
        with patch.object(window_input, "user32", self.api), \
             patch.object(window_input, "client_geometry", side_effect=geometry), \
             patch.object(window_input, "monotonic", side_effect=lambda: self.now), \
             patch.object(window_input, "sleep", side_effect=self.pause):
            return window_input.prepare_scan_window(123, (960, 3840), check_stop, timeout)

    def test_borderless_size_and_primary_top_alignment(self):
        result = self.prepare(lambda _hwnd: ((960, 3840), (800, 0)))
        style = self.api.SetWindowLongW.call_args.args[2] & 0xFFFFFFFF
        self.assertEqual(style & 0x00CF0000, 0)
        self.assertTrue(style & 0x80000000)
        self.assertEqual(self.api.SetWindowPos.call_args.args[2:6], (800, 0, 960, 3840))
        self.assertEqual(result["client_origin"], [800, 0])
        self.assertGreaterEqual(self.now, 0.25)

    def test_request_success_without_actual_resize_times_out(self):
        with self.assertRaisesRegex(RuntimeError, "设置无边框竖屏超时"):
            self.prepare(lambda _hwnd: ((1920, 1080), (0, 0)), timeout=0.3)
        self.api.SetWindowPos.assert_called_once()

    def test_cancel_before_changing_window(self):
        def cancel():
            raise RuntimeError("取消")

        with self.assertRaisesRegex(RuntimeError, "取消"):
            self.prepare(lambda _hwnd: ((960, 3840), (800, 0)), check_stop=cancel)
        self.api.SetWindowLongW.assert_not_called()
        self.api.SetWindowPos.assert_not_called()


if __name__ == "__main__":
    unittest.main()

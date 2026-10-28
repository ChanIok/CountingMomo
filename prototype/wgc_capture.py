"""WGC 回调只复制最新客户区帧，识别和稳定判断由消费者完成。"""

from dataclasses import dataclass
from threading import Condition
from time import monotonic

import numpy as np
from windows_capture import WindowsCapture

from window_input import capture_client_box, client_geometry


@dataclass
class CapturedFrame:
    sequence: int
    received_at: float
    source_time: int
    pixels: np.ndarray


class WindowCapture:
    def __init__(self, hwnd):
        self.hwnd = hwnd
        self.condition = Condition()
        self.latest = None
        self.error = None
        self.closed = False
        self.sequence = 0
        self.last_copy = 0.0
        self.frame_size = None
        self.box = None
        self.client_size = client_geometry(hwnd)[0]
        self.capture = WindowsCapture(window_hwnd=hwnd, cursor_capture=False)

        @self.capture.event
        def on_frame_arrived(frame, capture_control):
            now = monotonic()
            if now - self.last_copy < 0.08:
                return
            try:
                size = (frame.width, frame.height)
                if self.frame_size is None:
                    self.box = capture_client_box(hwnd, size)
                    self.frame_size = size
                elif size != self.frame_size:
                    raise RuntimeError("WGC 捕获尺寸发生变化，请重新扫描。")
                left, top, right, bottom = self.box
                # 原生映射只在回调期间有效，因此在此取得独立所有权。
                pixels = frame.frame_buffer[top:bottom, left:right].copy()
                with self.condition:
                    self.sequence += 1
                    self.latest = CapturedFrame(self.sequence, now, int(frame.timespan), pixels)
                    self.last_copy = now
                    self.condition.notify_all()
            except Exception as exc:
                with self.condition:
                    self.error = exc
                    self.condition.notify_all()
                capture_control.stop()

        @self.capture.event
        def on_closed():
            with self.condition:
                self.closed = True
                self.condition.notify_all()

        self.control = self.capture.start_free_threaded()

    def snapshot(self):
        with self.condition:
            if self.error:
                raise RuntimeError(f"WGC 捕获失败：{self.error}") from self.error
            if self.closed or self.control.is_finished():
                raise RuntimeError("WGC 捕获已结束，目标窗口可能已关闭。")
            return self.latest

    def close(self):
        self.control.stop()

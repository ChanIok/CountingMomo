"""一次采样事务：等待响应，再等待名称与星形前景在新帧中稳定。"""

from dataclasses import dataclass
from time import monotonic, sleep

import cv2
from PIL import Image

from scrollbar import detect_bottom
from tracking import (Page, PageNotReady, inputs_stable, locate_anchor, read_page,
                      reference_image, row_advance)


class PageWaitError(RuntimeError):
    def __init__(self, message, image, trace):
        super().__init__(message)
        self.image, self.trace = image, trace


@dataclass
class Observation:
    page: Page
    frame: dict
    displacement: int
    advance: int
    anchor: dict | None
    trace: list


def frame_info(frame):
    return {"sequence": frame.sequence, "received_at": frame.received_at,
            "source_time": frame.source_time}


def frame_image(frame):
    return Image.fromarray(cv2.cvtColor(frame.pixels, cv2.COLOR_BGRA2RGB))


def wait_frame(capture, check_stop, timeout, *, clock=monotonic, pause=sleep):
    deadline = clock() + timeout
    while clock() < deadline:
        check_stop()
        frame = capture.snapshot()
        if frame is not None:
            return frame
        pause(0.02)
    raise TimeoutError("等待首个 WGC 帧超时。")


def wait_page(capture, config, check_stop, stable_seconds, timeout, *, previous=None,
              anchor=None, after_sequence=0, clock=monotonic, pause=sleep):
    deadline = clock() + timeout
    last_sequence, last_source = after_sequence, None
    base, stable_since, stable_frames = None, None, 0
    base_displacement = None
    last_image, trace = None, []
    stage = "等待首屏" if previous is None else "等待滚动响应"
    while clock() < deadline:
        check_stop()
        frame = capture.snapshot()
        if frame is None or frame.sequence <= last_sequence:
            pause(0.02)
            continue
        last_sequence = frame.sequence
        # 同一原始帧即使重复回调也不能充当第二个稳定样本。
        if last_source is not None and frame.source_time <= last_source:
            pause(0.02)
            continue
        last_source = frame.source_time
        image = frame_image(frame)
        last_image = image
        now = clock()
        entry = frame_info(frame)
        match, displacement = None, 0
        reference = reference_image(image, config)
        bottom = detect_bottom(image, config)
        entry["at_bottom"] = bottom["at_bottom"]
        if previous is not None:
            match = locate_anchor(reference, anchor, config, bottom["at_bottom"])
            entry["anchor"] = match
            if not match["valid"]:
                stage = "等待名称锚点，尚不能确认接续"
                base, stable_since, stable_frames = None, None, 0
                entry["stage"] = stage
                trace.append(entry)
                continue
            displacement = match["displacement"]
            if displacement < -1:
                raise PageWaitError("名称锚点向下移动，与滚动方向不符。", image, trace + [entry])
            displacement = max(0, displacement)
            if displacement <= 1 and not bottom["at_bottom"]:
                stage = "等待滚动响应，未追加输入"
                base, stable_since, stable_frames = None, None, 0
                entry["stage"] = stage
                trace.append(entry)
                continue
        phase = (config["first_row_y"] if previous is None else previous.phase - displacement)
        try:
            page = read_page(image, config, phase, at_top=previous is None,
                             reference=reference, bottom=bottom)
        except PageNotReady as error:
            stage = str(error)
            base, stable_since, stable_frames = None, None, 0
            entry["stage"] = stage
            trace.append(entry)
            continue
        if base is None or displacement != base_displacement or not inputs_stable(base, page):
            base, stable_since, stable_frames = page, now, 1
            base_displacement = displacement
        else:
            stable_frames += 1
        stage = "等待名称与星形前景稳定"
        entry.update(stage=stage, displacement=displacement, stable_frames=stable_frames)
        trace.append(entry)
        # 只在另一个真实新帧到来时提交，不因墙钟时间过去就复用旧快照。
        if stable_frames >= 2 and now - stable_since >= stable_seconds:
            advance = row_advance(previous, page, displacement, config) if previous is not None else 0
            entry["stage"] = "就绪"
            return Observation(page, frame_info(frame), displacement, advance, match, trace)
    raise PageWaitError(f"采样事务超时：{stage}；未发送后续滚动输入。", last_image, trace)

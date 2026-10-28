"""一次小步标定后固定大步输入；不在扫描过程中持续学习。"""

from math import floor


MAX_WHEEL_NOTCHES = 32767 // 120
CALIBRATION_NOTCHES = 6


def target_displacement(config, overlap_rows):
    if overlap_rows < 2:
        raise ValueError("至少保留 2 行重叠。")
    top, bottom = config["scrolled_safe_top"], config["safe_y"][1]
    step, height = config["row_step"], config["card_size"][1]
    # 兼顾安全区高度、行相位与尾部重叠行，不依赖某一次画面的行位置。
    target = floor(min((bottom - top) * 0.8,
                       bottom - height - step - top - (overlap_rows - 1) * step))
    if target <= 0:
        raise ValueError("安全区域不足以保留指定重叠行数。")
    return target


def calibrated_notches(displacement, target):
    if displacement <= 1:
        raise RuntimeError("小步滚动未产生可测位移，不能完成标定。")
    rate = displacement / CALIBRATION_NOTCHES
    return max(1, min(MAX_WHEEL_NOTCHES, floor(target / rate))), rate


def verify_response(displacement, expected, at_bottom, row_step):
    if not at_bottom and abs(displacement - expected) > row_step:
        raise RuntimeError(f"滚动响应与一次标定不符：预期 {expected:.0f}px，实测 {displacement}px；停止并保留现场，不继续追加输入。")

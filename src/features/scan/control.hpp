#pragma once

#include "vendor/std.hpp"

namespace core {
struct AppState;
}

namespace features::scan {
// 设置停止标志并唤醒正在等帧的扫描线程。
auto request_stop(core::AppState& state) -> void;
// 保留工作线程的 F8 检测，UI 繁忙时仍能停止游戏输入。
auto stop_requested(core::AppState& state) -> bool;
}  // namespace features::scan

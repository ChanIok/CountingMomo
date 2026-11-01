#pragma once

#include "vendor/std.hpp"

namespace core {
struct AppState;
}

namespace features::scan {
inline constexpr int stop_hotkey_id = 1;
// 空闲时短暂检查，不持有按键；扫描期间由 start/consume_updates 管理注册。
auto check_hotkey(core::AppState& state) -> void;
auto register_hotkey(core::AppState& state) -> void;
auto release_hotkey(core::AppState& state) -> void;
// 设置停止标志并唤醒正在等帧的扫描线程。
auto request_stop(core::AppState& state) -> void;
// 工作线程只检测本次已成功注册的停止键，UI 繁忙时仍能停止游戏输入。
auto stop_requested(core::AppState& state) -> bool;
}  // namespace features::scan

#pragma once

#include "vendor/std.hpp"

#include "core/message/types.hpp"
#include "features/scan/task_types.hpp"

namespace core {
struct AppState;
}

namespace features::scan {

// 以下入口均由 UI 线程调用；工作线程只通过同步槽位交付进度与终态。
auto running(const core::AppState& state) -> bool;
auto start(core::AppState& state) -> void;
auto stop(core::AppState& state) -> void;
auto consume_updates(core::AppState& state) -> void;
auto open_diagnostic(core::AppState& state) -> void;

}  // namespace features::scan

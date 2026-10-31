#pragma once

#include "vendor/std.hpp"

namespace core {
struct AppState;
}

namespace features::scan {
// 复制工作线程快照，同一时间最多挂起一条刷新通知。
auto publish_progress(core::AppState& state) -> void;
}  // namespace features::scan

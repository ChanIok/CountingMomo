#pragma once

#include "vendor/std.hpp"

namespace core {
struct AppState;
}

namespace core::shutdown {
// 由 UI 线程在消息循环结束或启动失败后调用；允许各模块只完成了部分初始化。
auto shutdown_application(AppState& state) -> void;
}  // namespace core::shutdown

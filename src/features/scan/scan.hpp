#pragma once

#include "vendor/std.hpp"

#include "core/message/types.hpp"

namespace core {
struct AppState;
}

namespace features::scan {
// 同步采集当前分类，页面、累计数组和资源均由 AppState 持有。
auto collect_category(core::AppState& state) -> std::expected<void, core::Error>;
}  // namespace features::scan

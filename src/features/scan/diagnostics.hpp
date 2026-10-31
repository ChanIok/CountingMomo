#pragma once

#include "vendor/std.hpp"

#include "core/message/types.hpp"

namespace core {
struct AppState;
}

namespace features::scan {
struct Outcome;
}

namespace features::scan::diagnostics {
// 仅异常终态保存报告与已有现场图；捕获已关闭，工作数据和线程 COM 套间仍有效。
auto save(core::AppState& state, Outcome& outcome) -> std::expected<void, core::Error>;
}  // namespace features::scan::diagnostics

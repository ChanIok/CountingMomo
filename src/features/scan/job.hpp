#pragma once

#include "vendor/std.hpp"

#include "core/message/types.hpp"
#include "features/scan/state.hpp"
#include "features/scan/task_types.hpp"

namespace core {
struct AppState;
}

namespace features::scan::job {

// 任务线程边界：执行主流程、把中断转成 Outcome，并完成收尾与窗口恢复。
auto execute(core::AppState& state) -> Outcome;

auto start(core::AppState& state, Request request) -> std::expected<void, core::Error>;

// UI 线程消费进度并在收到终态后回收工作线程。
auto consume(core::AppState& state) -> std::optional<Outcome>;

// 请求停止并等待任务收尾，必须先于窗口与 AppState 子状态销毁。
auto shutdown(core::AppState& state) -> void;

}  // namespace features::scan::job

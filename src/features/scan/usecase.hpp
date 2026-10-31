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
// 仅任务线程调用；请求已冻结在扫描状态中，返回前释放会话并恢复窗口。
auto run(core::AppState& state) -> std::expected<Outcome, core::Error>;
// 仅任务线程调用；保存异常诊断后释放工作数据，再撤销线程 COM 套间。
auto release_run(core::AppState& state) -> void;
}  // namespace features::scan

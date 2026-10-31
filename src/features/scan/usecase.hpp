#pragma once
#include "vendor/std.hpp"

#include "core/message/message.hpp"

#include "features/scan/request.hpp"
#include "features/scan/state.hpp"

namespace core {
struct AppState;
}

namespace features::scan {
// 同步执行完整分类扫描；调用者负责本次停止状态与 COM 初始化。
// 只有完整扫描成功的 categories 可作为本次完整结果发布。
auto run(core::AppState& state, const Request& request) -> std::expected<Outcome, core::Error>;
}  // namespace features::scan

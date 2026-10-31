#pragma once
#include "vendor/std.hpp"

#include "core/message/message.hpp"

#include "features/scan/request.hpp"
#include "features/scan/state.hpp"

namespace core {
struct AppState;
}

namespace app::scan_job {
auto start(core::AppState& state, features::scan::Request request)
    -> std::expected<void, core::Error>;
// UI 线程读取最新进度并接收终态，终态不参与进度合并。
auto consume(core::AppState& state) -> std::optional<features::scan::Outcome>;
}  // namespace app::scan_job

#pragma once
#include "vendor/std.hpp"

#include "core/message/message.hpp"

namespace core {
struct AppState;
}

namespace app {
auto initialize(core::AppState& state) -> std::expected<void, core::Error>;
auto running(const core::AppState& state) -> bool;
auto start_scan(core::AppState& state) -> void;
auto stop_scan(core::AppState& state) -> void;
auto refresh(core::AppState& state) -> void;
auto copy_result(core::AppState& state) -> void;
auto save_result(core::AppState& state) -> void;
auto open_diagnostic(core::AppState& state) -> void;
auto save_settings(core::AppState& state) -> void;
auto close(core::AppState& state) -> void;
}  // namespace app

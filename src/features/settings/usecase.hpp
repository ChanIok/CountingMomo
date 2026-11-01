#pragma once

#include "vendor/std.hpp"

#include "core/message/types.hpp"

namespace core {
struct AppState;
}

namespace features::settings {
auto initialize(core::AppState& state) -> std::expected<void, core::Error>;
auto save(core::AppState& state) -> void;
auto cycle_theme(core::AppState& state) -> std::expected<void, core::Error>;
auto toggle_language(core::AppState& state) -> void;
auto set_stop_key(core::AppState& state, int key) -> void;
auto open_repository(core::AppState& state) -> void;
}  // namespace features::settings

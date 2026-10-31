#pragma once
#include "vendor/std.hpp"

#include "core/message/message.hpp"

namespace core {
struct AppState;
}

namespace ui::status_window {
auto create(core::AppState& state) -> std::expected<void, core::Error>;
auto refresh(core::AppState& state, bool finished = false) -> void;
auto hide(core::AppState& state) -> void;
}  // namespace ui::status_window

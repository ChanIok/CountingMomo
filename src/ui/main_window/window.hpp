#pragma once
#include "vendor/std.hpp"

#include "vendor/windows.hpp"

#include "core/message/message.hpp"

namespace core {
struct AppState;
}

namespace ui::main_window {
inline constexpr UINT scan_update = WM_APP + 1;
inline constexpr UINT theme_update = WM_APP + 2;
auto create(core::AppState& state) -> std::expected<void, core::Error>;
auto refresh(core::AppState& state) -> void;
auto show(core::AppState& state) -> void;
auto update_theme(core::AppState& state) -> std::expected<void, core::Error>;
}  // namespace ui::main_window

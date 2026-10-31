#pragma once

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

#include "core/message/message.hpp"

namespace core {
struct AppState;
}

namespace ui::main_window {
inline constexpr auto class_name = L"CountingMomoMainWindow";
inline constexpr UINT scan_update = WM_APP + 1;
inline constexpr UINT theme_update = WM_APP + 2;
inline constexpr UINT activate_instance = WM_APP + 3;
auto activate_existing() -> std::expected<void, core::Error>;
auto notify(core::AppState& state, core::Error message) -> void;
auto close(core::AppState& state) -> void;
// 消息循环结束后由应用 shutdown 调用，先关闭输入入口，再单独销毁窗口。
auto prepare_shutdown(core::AppState& state) -> void;
auto destroy(core::AppState& state) -> void;
auto create(core::AppState& state) -> std::expected<void, core::Error>;
auto refresh(core::AppState& state) -> void;
auto show(core::AppState& state) -> void;
auto update_theme(core::AppState& state) -> std::expected<void, core::Error>;
auto update_language(core::AppState& state) -> void;
}  // namespace ui::main_window

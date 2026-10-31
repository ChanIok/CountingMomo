#pragma once
#include "vendor/std.hpp"

#include "vendor/windows.hpp"
#include "vendor/windows/d2d1_1.hpp"

#include "core/message/message.hpp"

namespace core {
struct AppState;
}

namespace ui::theme {
struct Palette {
  bool dark{};
  bool high_contrast{};
  bool transparency{};
  D2D1_COLOR_F background;
  D2D1_COLOR_F card;
  D2D1_COLOR_F text;
  D2D1_COLOR_F muted;
  D2D1_COLOR_F line;
  D2D1_COLOR_F accent;
  D2D1_COLOR_F accent_hover;
  D2D1_COLOR_F accent_pressed;
  D2D1_COLOR_F accent_text;
  D2D1_COLOR_F secondary;
  D2D1_COLOR_F secondary_hover;
  D2D1_COLOR_F secondary_pressed;
  D2D1_COLOR_F secondary_border;
  D2D1_COLOR_F secondary_text;
  D2D1_COLOR_F soft;
  D2D1_COLOR_F soft_hover;
  D2D1_COLOR_F soft_pressed;
  D2D1_COLOR_F danger;
};

// 在 RoInitialize 和读取应用设置后初始化。
auto initialize(core::AppState& state) -> std::expected<void, core::Error>;
// 仅向目标窗口投递消息，回调不访问 AppState 或图形设备。
auto watch(core::AppState& state, HWND target, UINT message) -> std::expected<void, core::Error>;
// 在 UI 线程重新读取系统设置，成功后一次性替换色板。
auto refresh(core::AppState& state) -> std::expected<void, core::Error>;
// 销毁目标窗口前调用；同时阻止在途回调投递，允许重复调用。
auto shutdown(core::AppState& state) -> void;
// 读取已初始化的色板，不查询系统或重建绘图资源。
auto resolve(const core::AppState& state) -> const Palette&;
}  // namespace ui::theme

#pragma once

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

namespace ui::main_window::frame {
inline constexpr float title_bar_height = 32;

// 标题栏留空并保留系统按钮；应用页头在下方客户区单独绘制。
auto process(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) -> std::optional<LRESULT>;
// width、height 为客户区 DIP，返回供 CreateWindow／最小尺寸使用的像素矩形。
auto outer_rect(int width, int height, UINT dpi) -> RECT;
auto extend(HWND hwnd, bool material) -> void;
}  // namespace ui::main_window::frame

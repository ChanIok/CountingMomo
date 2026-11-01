#pragma once

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

#include "ui/theme.hpp"

namespace ui::backdrop {
// 统一应用 DWM 材质、边框扩展和主窗口原生标题栏外观，返回材质是否实际启用。
// transient 保留无标题浮窗的 Acrylic 外观，不应用主窗口标题栏配色。
auto apply(HWND hwnd, const theme::Palette& colors, bool transient = false) -> bool;
}  // namespace ui::backdrop

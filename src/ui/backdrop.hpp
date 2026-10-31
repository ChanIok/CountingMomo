#pragma once
#include "vendor/std.hpp"

#include "vendor/windows.hpp"

#include "ui/theme.hpp"

namespace ui::backdrop {
// 返回是否实际启用了系统材质；关闭透明效果时使用主题纯色。
auto apply(HWND hwnd, const theme::Palette& colors, bool transient = false) -> bool;
}  // namespace ui::backdrop

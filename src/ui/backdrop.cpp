#include "ui/backdrop.hpp"

#include "vendor/std.hpp"

#include "vendor/windows/dwmapi.hpp"

namespace ui::backdrop {
// 为指定窗口应用 DWM 系统级背景材质：设置沉浸暗色 → 设置圆角类型 → 开启 Mica/Acrylic
// 材质并扩展边框至客户区
auto apply(HWND hwnd, const theme::Palette& colors, bool transient) -> bool {
  // 设置 DWM 沉浸式暗色模式标记
  BOOL dark = colors.dark;
  DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
  // 浮窗使用小圆角，主窗口使用标准大圆角
  auto corner = transient ? DWMWCP_ROUNDSMALL : DWMWCP_ROUND;
  DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
  // 配置系统 Mica / Acrylic 材质
  auto material =
      colors.transparency ? (transient ? DWMSBT_TRANSIENTWINDOW : DWMSBT_MAINWINDOW) : DWMSBT_NONE;
  bool enabled = SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &material,
                                                 sizeof(material))) &&
                 material != DWMSBT_NONE;
  // 将 DWM 材质边框完整扩展到窗口客户区以呈现透明毛玻璃
  MARGINS margins = enabled ? MARGINS{-1, -1, -1, -1} : MARGINS{};
  if (FAILED(DwmExtendFrameIntoClientArea(hwnd, &margins))) {
    enabled = false;
  }
  return enabled;
}
}  // namespace ui::backdrop

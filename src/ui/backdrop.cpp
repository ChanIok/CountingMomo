#include "ui/backdrop.hpp"

#include "vendor/std.hpp"

#include "vendor/windows/dwmapi.hpp"

namespace ui::backdrop {
auto apply(HWND hwnd, const theme::Palette& colors, bool transient) -> bool {
  BOOL dark = colors.dark;
  DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
  auto corner = transient ? DWMWCP_ROUNDSMALL : DWMWCP_ROUND;
  DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
  auto material =
      colors.transparency ? (transient ? DWMSBT_TRANSIENTWINDOW : DWMSBT_MAINWINDOW) : DWMSBT_NONE;
  bool enabled = SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &material,
                                                 sizeof(material))) &&
                 material != DWMSBT_NONE;
  MARGINS margins = enabled ? MARGINS{-1, -1, -1, -1} : MARGINS{};
  if (FAILED(DwmExtendFrameIntoClientArea(hwnd, &margins))) {
    enabled = false;
  }
  return enabled;
}
}  // namespace ui::backdrop

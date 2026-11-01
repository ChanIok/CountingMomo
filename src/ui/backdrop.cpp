#include "ui/backdrop.hpp"

#include "vendor/std.hpp"

#include "vendor/windows/dwmapi.hpp"
#include "vendor/windows/uxtheme.hpp"

namespace ui::backdrop {
namespace {
auto color_ref(const D2D1_COLOR_F& color) -> COLORREF {
  return RGB(static_cast<BYTE>(std::lround(color.r * 255)),
             static_cast<BYTE>(std::lround(color.g * 255)),
             static_cast<BYTE>(std::lround(color.b * 255)));
}
}  // namespace

auto apply(HWND hwnd, const theme::Palette& colors, bool transient) -> bool {
  BOOL dark = colors.dark && !colors.high_contrast;
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
    // 客户区无法显露材质时，同时撤销系统背景，保持纯色模式一致。
    if (enabled) {
      material = DWMSBT_NONE;
      DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &material, sizeof(material));
    }
    enabled = false;
  }
  if (!transient) {
    // 应用页头已显示品牌；只隐藏标题栏图标与文字的绘制，保留其数据供 Shell 使用。
    constexpr DWORD hidden = WTNCA_NODRAWCAPTION | WTNCA_NODRAWICON;
    SetWindowThemeNonClientAttributes(hwnd, hidden, hidden);
    // 材质与高对比度交由系统着色；每次显式重置，避免残留上一次的纯色。
    bool solid = !enabled && !colors.high_contrast;
    COLORREF caption = solid ? color_ref(colors.background) : DWMWA_COLOR_DEFAULT;
    COLORREF text = solid ? color_ref(colors.text) : DWMWA_COLOR_DEFAULT;
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &text, sizeof(text));
  }
  return enabled;
}
}  // namespace ui::backdrop

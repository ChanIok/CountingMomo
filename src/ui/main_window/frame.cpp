#include "ui/main_window/frame.hpp"

#include "vendor/std.hpp"

#include "vendor/windows/dwmapi.hpp"

namespace ui::main_window::frame {
// 处理主窗口非客户区消息：计算无边框客户区（WM_NCCALCSIZE）与非客户区光标命中（WM_NCHITTEST）
auto process(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) -> std::optional<LRESULT> {
  // 自定义非客户区大小：去掉原生标题栏并将客户区扩展至顶部
  if (message == WM_NCCALCSIZE && wparam) {
    auto& rect = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0];
    LONG top = rect.top;
    // 左右、底边和原生滚动条仍由系统计算，只去掉顶部的标准图标、文字区域。
    DefWindowProcW(hwnd, message, wparam, lparam);
    rect.top = top;
    // 最大化时需下移避开屏幕外溢的调整边框
    if (IsZoomed(hwnd)) {
      UINT dpi = GetDpiForWindow(hwnd);
      rect.top += GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) +
                  GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    }
    return 0;
  }
  LRESULT result{};
  // 包括 WM_NCMOUSELEAVE，系统按钮相关消息先交给 DWM。
  if (DwmDefWindowProc(hwnd, message, wparam, lparam, &result)) {
    return result;
  }
  // 仅在非客户区命中测试消息时进行后续判定
  if (message != WM_NCHITTEST) {
    return {};
  }
  POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
  // 自定义客户区在最大化时可能使 DWM 不返回按钮命中；使用系统按钮的屏幕像素矩形。
  TITLEBARINFOEX title_bar{sizeof(TITLEBARINFOEX)};
  SendMessageW(hwnd, WM_GETTITLEBARINFOEX, 0, reinterpret_cast<LPARAM>(&title_bar));
  constexpr std::array<LRESULT, 6> button_hits{HTNOWHERE,   HTNOWHERE, HTMINBUTTON,
                                               HTMAXBUTTON, HTHELP,    HTCLOSE};
  // 优先判定原生右上角最小化/最大化/关闭按钮区域
  for (std::size_t index = 2; index < button_hits.size(); ++index) {
    if (!(title_bar.rgstate[index] & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN)) &&
        PtInRect(&title_bar.rgrect[index], point)) {
      return button_hits[index];
    }
  }
  result = DefWindowProcW(hwnd, message, wparam, lparam);
  // 仅接管客户区，保留系统按钮和边框的命中，避免把还原按钮改成标题栏。
  if (result != HTCLIENT) {
    return result;
  }
  ScreenToClient(hwnd, &point);
  UINT dpi = GetDpiForWindow(hwnd);
  int top_border =
      GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
  // 顶端可拉伸边框
  if (!IsZoomed(hwnd) && point.y >= 0 && point.y < top_border) {
    return HTTOP;
  }
  // 自定义标题栏高度内返回 HTCAPTION 以支持窗口拖动与双击最大化
  if (point.y >= 0 && point.y < title_bar_height * dpi / 96.0f) {
    return HTCAPTION;
  }
  return HTCLIENT;
}

// 计算包含标准系统边框的窗口物理外框矩形尺寸
auto outer_rect(int width, int height, UINT dpi) -> RECT {
  RECT rect{0, 0, MulDiv(width, dpi, 96), MulDiv(height, dpi, 96)};
  // 计算 WS_OVERLAPPEDWINDOW 样式所需边框补偿
  AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
  rect.top = 0;
  return rect;
}

// 扩展 DWM 磨砂边框至客户区：启用材质时覆盖全窗，未启用时仅扩展标题栏高度
auto extend(HWND hwnd, bool material) -> void {
  // 材质模式下将毛玻璃边界铺满窗口客户区
  MARGINS margins =
      material
          ? MARGINS{-1, -1, -1, -1}
          : MARGINS{0, 0, MulDiv(static_cast<int>(title_bar_height), GetDpiForWindow(hwnd), 96), 0};
  DwmExtendFrameIntoClientArea(hwnd, &margins);
}
}  // namespace ui::main_window::frame

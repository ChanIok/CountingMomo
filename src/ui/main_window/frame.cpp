#include "ui/main_window/frame.hpp"

#include "vendor/std.hpp"

#include "vendor/windows/dwmapi.hpp"

namespace ui::main_window::frame {
auto process(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) -> std::optional<LRESULT> {
  if (message == WM_NCCALCSIZE && wparam) {
    auto& rect = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0];
    LONG top = rect.top;
    // 左右、底边和原生滚动条仍由系统计算，只去掉顶部的标准图标、文字区域。
    DefWindowProcW(hwnd, message, wparam, lparam);
    rect.top = top;
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
  if (message != WM_NCHITTEST) {
    return {};
  }
  POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
  // 自定义客户区在最大化时可能使 DWM 不返回按钮命中；使用系统按钮的屏幕像素矩形。
  TITLEBARINFOEX title_bar{sizeof(TITLEBARINFOEX)};
  SendMessageW(hwnd, WM_GETTITLEBARINFOEX, 0, reinterpret_cast<LPARAM>(&title_bar));
  constexpr std::array<LRESULT, 6> button_hits{HTNOWHERE, HTNOWHERE, HTMINBUTTON, HTMAXBUTTON,
                                             HTHELP, HTCLOSE};
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
  if (!IsZoomed(hwnd) && point.y >= 0 && point.y < top_border) {
    return HTTOP;
  }
  if (point.y >= 0 && point.y < title_bar_height * dpi / 96.0f) {
    return HTCAPTION;
  }
  return HTCLIENT;
}

auto outer_rect(int width, int height, UINT dpi) -> RECT {
  RECT rect{0, 0, MulDiv(width, dpi, 96), MulDiv(height, dpi, 96)};
  AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
  rect.top = 0;
  return rect;
}

auto extend(HWND hwnd, bool material) -> void {
  MARGINS margins =
      material
          ? MARGINS{-1, -1, -1, -1}
          : MARGINS{0, 0, MulDiv(static_cast<int>(title_bar_height), GetDpiForWindow(hwnd), 96), 0};
  DwmExtendFrameIntoClientArea(hwnd, &margins);
}
}  // namespace ui::main_window::frame

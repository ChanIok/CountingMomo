#include "ui/status_window/window.hpp"

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

#include "core/message/message.hpp"

#include "app/state.hpp"
#include "app/usecase.hpp"
#include "core/i18n/i18n.hpp"
#include "core/state/app_state.hpp"
#include "ui/backdrop.hpp"
#include "ui/button.hpp"
#include "ui/composition.hpp"
#include "ui/main_window/window.hpp"
#include "ui/status_window/painter.hpp"
#include "ui/status_window/state.hpp"
#include "ui/theme.hpp"
#include "utils/string.hpp"

namespace ui::status_window {
namespace {
auto position(core::AppState& state, bool active) -> std::optional<RECT> {
  std::vector<RECT> monitors;
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        MONITORINFO info{sizeof(MONITORINFO)};
        if (GetMonitorInfoW(monitor, &info)) {
          reinterpret_cast<std::vector<RECT>*>(data)->push_back(info.rcWork);
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&monitors));
  RECT game{};
  if (active && !GetWindowRect(state.app->game, &game)) {
    return {};
  }
  UINT dpi = state.status_window->dpi;
  int width = MulDiv(380, dpi, 96);
  int height = MulDiv(140, dpi, 96);
  int margin = MulDiv(16, dpi, 96);
  for (const auto& monitor : monitors) {
    for (auto point : {POINT{monitor.right - width - margin, monitor.bottom - height - margin},
                       POINT{monitor.left + margin, monitor.bottom - height - margin},
                       POINT{monitor.right - width - margin, monitor.top + margin},
                       POINT{monitor.left + margin, monitor.top + margin}}) {
      RECT candidate{point.x, point.y, point.x + width, point.y + height};
      RECT overlap{};
      if (candidate.left < monitor.left || candidate.top < monitor.top ||
          candidate.right > monitor.right || candidate.bottom > monitor.bottom) {
        continue;
      }
      // 不遮挡游戏的任何区域；没有空位时不显示扫描浮层，F8 和主面板仍可使用。
      if (!active || !IntersectRect(&overlap, &candidate, &game)) {
        return candidate;
      }
    }
  }
  return {};
}

auto failed(core::AppState& state, core::Error message) -> void {
  state.status_window->graphics_failed = true;
  hide(state);
  state.app->notice = std::move(message);
  state.app->notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  ui::main_window::refresh(state);
}

auto paint(core::AppState& state) -> std::expected<void, core::Error> {
  auto painted = painter::paint(state);
  if (!painted) {
    auto recovered = composition::recover(state);
    if (!recovered) {
      return std::unexpected(recovered.error());
    }
    if (*recovered) {
      return painter::paint(state);
    }
  }
  return painted;
}

auto procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT {
  auto state = reinterpret_cast<core::AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    state = static_cast<core::AppState*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    state->status_window->hwnd = hwnd;
  }
  if (!state) {
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }
  auto& window = *state->status_window;
  switch (message) {
    case WM_CREATE:
      window.dpi = GetDpiForWindow(hwnd);
      window.material = backdrop::apply(hwnd, theme::resolve(*state), true);
      return 0;
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_MOUSEMOVE:
    case WM_MOUSELEAVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP: {
      // WM_PAINT 优先级低于输入；先应用待处理的内容/布局，避免点击旧页面的按钮。
      if (window.redraw || window.repaint_buttons) {
        if (auto painted = paint(*state); !painted) {
          failed(*state, painted.error());
          return 0;
        }
      }
      auto hovered = window.buttons.hovered;
      auto pressed = window.buttons.pressed;
      auto action = button::pointer(*state, window.buttons, hwnd, message, lparam, window.dpi);
      if (!action) {
        failed(*state, action.error());
        return 0;
      }
      if (hovered != window.buttons.hovered || pressed != window.buttons.pressed) {
        if (auto committed = composition::commit(*state); !committed) {
          failed(*state, committed.error());
          return 0;
        }
      }
      if (*action == button::Action::copy) {
        app::copy_result(*state);
      } else if (*action == button::Action::view) {
        ui::main_window::show(*state);
        hide(*state);
      }
      return 0;
    }
    case WM_CAPTURECHANGED:
      if (window.buttons.pressed == button::Action::none) {
        return 0;
      }
      [[fallthrough]];
    case WM_CANCELMODE:
      if (auto cancelled = button::cancel(*state, window.buttons, hwnd); !cancelled) {
        failed(*state, cancelled.error());
        return 0;
      }
      if (window.composition.target) {
        if (auto committed = composition::commit(*state); !committed) {
          failed(*state, committed.error());
          return 0;
        }
      }
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT native_paint{};
      BeginPaint(hwnd, &native_paint);
      composition::clear_backing(hwnd, native_paint.hdc);
      std::expected<void, core::Error> painted;
      if (!window.graphics_failed && (window.redraw || window.repaint_buttons)) {
        painted = paint(*state);
      }
      EndPaint(hwnd, &native_paint);
      if (!painted) {
        failed(*state, painted.error());
      }
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_DPICHANGED:
      window.dpi = HIWORD(wparam);
      window.redraw = true;
      window.repaint_buttons = true;
      refresh(*state);
      return 0;
    case WM_DESTROY:
      // 先清空状态，避免释放捕获时同步的 WM_CAPTURECHANGED 再更新退场动画。
      window.buttons = {};
      if (GetCapture() == hwnd) {
        ReleaseCapture();
      }
      window.indicator = {};
      window.composition = {};
      return 0;
    case WM_NCDESTROY:
      window.hwnd = nullptr;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      break;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

auto CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT {
  try {
    return procedure(hwnd, message, wparam, lparam);
  } catch (const std::exception& error) {
    auto state = reinterpret_cast<core::AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (state) {
      state->status_window->graphics_failed = true;
      hide(*state);
      state->app->notice = core::with_detail(core::error("error.ui.exception"), error.what());
      state->app->notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    }
    return 0;
  }
}
}  // namespace

auto create(core::AppState& state) -> std::expected<void, core::Error> {
  WNDCLASSEXW definition{sizeof(WNDCLASSEXW)};
  definition.hInstance = GetModuleHandleW(nullptr);
  definition.lpfnWndProc = window_proc;
  definition.lpszClassName = L"CountingMomoStatusWindow";
  definition.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  if (!RegisterClassExW(&definition)) {
    return std::unexpected(core::error("error.ui.register_status"));
  }
  auto hwnd =
      CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, definition.lpszClassName,
                      core::i18n::wide(state, "app.status_title").c_str(), WS_POPUP, 0, 0, 380, 140,
                      nullptr, nullptr, definition.hInstance, &state);
  if (!hwnd) {
    return std::unexpected(core::error("error.ui.create_status"));
  }
  return {};
}

auto hide(core::AppState& state) -> void {
  auto& window = *state.status_window;
  window.expires = {};
  if (window.hwnd && window.visible) {
    if (auto cancelled = button::cancel(state, window.buttons, window.hwnd); !cancelled) {
      state.app->notice = cancelled.error();
      state.app->notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    }
    if (window.indicator.node.visual) {
      auto stopped = composition::pulse(state, window.indicator.node, false);
      auto committed = stopped ? composition::commit(state) : stopped;
      if (!committed) {
        state.app->notice = committed.error();
        state.app->notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      }
    }
    ShowWindow(window.hwnd, SW_HIDE);
  }
  window.visible = false;
}

auto refresh(core::AppState& state, bool finished) -> void {
  auto& window = *state.status_window;
  if (!window.hwnd || window.graphics_failed) {
    return;
  }
  bool active = app::running(state);
  auto now = std::chrono::steady_clock::now();
  if (finished) {
    window.expires = now + std::chrono::seconds(12);
  }
  if (!active && (!window.visible && !finished)) {
    return;
  }
  if (state.app->closing || (!active && now >= window.expires)) {
    hide(state);
    return;
  }
  auto rect = position(state, active);
  if (!rect) {
    hide(state);
    return;
  }
  bool success = !active && state.app->phase == app::Phase::completed;
  bool failed = state.app->phase == app::Phase::failed;
  std::wstring title = active    ? (state.app->phase == app::Phase::stopping
                                        ? core::i18n::wide(state, "scan.stopping_cleanup")
                                        : core::i18n::wide(state, "scan.running"))
                       : success ? core::i18n::wide(state, "scan.completed")
                       : state.app->phase == app::Phase::stopped
                           ? core::i18n::wide(state, "scan.stopped")
                           : core::i18n::wide(state, "scan.failed");
  if (active && !state.app->progress.category.empty()) {
    title += L" · " + core::i18n::wide(state, "category." + state.app->progress.category);
  }
  std::wstring detail;
  if (active) {
    const auto& progress = state.app->progress;
    if (progress.category.empty()) {
      detail = core::i18n::wide(state, features::scan::stage_message(progress.stage));
    } else {
      detail = core::i18n::wide(state, "scan.progress", progress.completed_categories,
                                progress.planned_categories, progress.category_count);
    }
  } else if (success && state.app->success) {
    detail = core::i18n::wide(state, "scan.results_ready", state.app->success->categories.size());
  } else {
    detail = core::i18n::wide(state, "scan.open_for_details");
  }
  if (window.title != title || window.detail != detail || window.active != active ||
      window.success != success || window.copied != state.app->copied || window.failed != failed) {
    window.title = std::move(title);
    window.detail = std::move(detail);
    window.active = active;
    window.success = success;
    window.copied = state.app->copied;
    window.failed = failed;
    window.redraw = true;
  }
  RECT current{};
  GetWindowRect(window.hwnd, &current);
  if (!window.visible || current.left != rect->left || current.top != rect->top ||
      current.right != rect->right || current.bottom != rect->bottom) {
    SetWindowPos(window.hwnd, HWND_TOPMOST, rect->left, rect->top, rect->right - rect->left,
                 rect->bottom - rect->top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  }
  if (!window.visible) {
    window.redraw = true;
  }
  window.visible = true;
  if (window.redraw || window.repaint_buttons) {
    InvalidateRect(window.hwnd, nullptr, FALSE);
  }
}

}  // namespace ui::status_window

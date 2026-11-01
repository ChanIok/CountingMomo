#include "ui/status_window/window.hpp"

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

#include "core/i18n/i18n.hpp"
#include "core/message/message.hpp"
#include "core/state/app_state.hpp"
#include "features/result/state.hpp"
#include "features/result/usecase.hpp"
#include "features/scan/state.hpp"
#include "features/scan/usecase.hpp"
#include "features/window/state.hpp"
#include "ui/backdrop.hpp"
#include "ui/button.hpp"
#include "ui/composition.hpp"
#include "ui/main_window/state.hpp"
#include "ui/main_window/window.hpp"
#include "ui/status_window/painter.hpp"
#include "ui/status_window/state.hpp"
#include "ui/theme.hpp"
#include "utils/string.hpp"

namespace ui::status_window {
namespace {
// 固定在锚点窗口所在显示器的工作区右下角，允许覆盖游戏。
auto position(HWND anchor, UINT dpi) -> std::optional<RECT> {
  MONITORINFO monitor{sizeof(MONITORINFO)};
  if (!GetMonitorInfoW(MonitorFromWindow(anchor, MONITOR_DEFAULTTONEAREST), &monitor)) {
    return {};
  }
  int width = MulDiv(380, dpi, 96);
  int height = MulDiv(140, dpi, 96);
  int margin = MulDiv(16, dpi, 96);
  return RECT{monitor.rcWork.right - width - margin, monitor.rcWork.bottom - height - margin,
              monitor.rcWork.right - margin, monitor.rcWork.bottom - margin};
}

// 记录浮窗渲染失败状态，隐藏浮窗并向主窗口发布临时错误通知
auto failed(core::AppState& state, core::Error message) -> void {
  state.status_window->graphics_failed = true;
  hide(state);
  state.main_window->notice = std::move(message);
  state.main_window->notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  ui::main_window::refresh(state);
}

// 执行悬浮窗重绘并在显卡设备丢失时尝试自动恢复重建
auto paint(core::AppState& state) -> std::expected<void, core::Error> {
  auto painted = painter::paint(state);
  if (!painted) {
    auto recovered = composition::recover(state);
    if (!recovered) {
      return std::unexpected(recovered.error());
    }
    // 恢复成功后二次绘制
    if (*recovered) {
      return painter::paint(state);
    }
  }
  return painted;
}

// 悬浮状态窗 Win32 消息处理：初始化无焦点工具窗、按钮交互与重绘
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
      // 读取 DPI 并应用 Acrylic 亚克力背景材质
      window.dpi = GetDpiForWindow(hwnd);
      window.material = backdrop::apply(hwnd, theme::resolve(*state), true);
      return 0;
    case WM_MOUSEACTIVATE:
      // 保持无焦点状态，防止夺取全屏/独占游戏窗口的输入焦点
      return MA_NOACTIVATE;
    case WM_MOUSEMOVE:
    case WM_MOUSELEAVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP: {
      if (state->main_window->closing) {
        return 0;
      }
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
      // 响应悬浮窗按钮动作
      if (*action == button::Action::copy) {
        features::result::copy(*state);
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
      window.visible = false;
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

// 顶层 Win32 窗口过程包装：捕获运行时异常以安全隐藏浮窗并上报错误
auto CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT {
  try {
    return procedure(hwnd, message, wparam, lparam);
  } catch (const std::exception& error) {
    auto state = reinterpret_cast<core::AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (state) {
      state->status_window->graphics_failed = true;
      hide(*state);
      state->main_window->notice =
          core::with_detail(core::error("error.ui.exception"), error.what());
      state->main_window->notice_until =
          std::chrono::steady_clock::now() + std::chrono::seconds(10);
    }
    return 0;
  }
}
}  // namespace

// 创建无焦点顶层工具窗口（WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE）
auto create(core::AppState& state) -> std::expected<void, core::Error> {
  // 注册悬浮窗类
  WNDCLASSEXW definition{sizeof(WNDCLASSEXW)};
  definition.hInstance = GetModuleHandleW(nullptr);
  definition.lpfnWndProc = window_proc;
  definition.lpszClassName = L"CountingMomoStatusWindow";
  definition.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  if (!RegisterClassExW(&definition)) {
    return std::unexpected(core::error("error.ui.register_status"));
  }
  // 创建无边框且置顶的 POPUP 工具窗口
  auto hwnd =
      CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, definition.lpszClassName,
                      core::i18n::wide(state, "app.status_title").c_str(), WS_POPUP, 0, 0, 380, 140,
                      nullptr, nullptr, definition.hInstance, &state);
  if (!hwnd) {
    return std::unexpected(core::error("error.ui.create_status"));
  }
  return {};
}

auto destroy(core::AppState& state) -> void {
  if (state.status_window->hwnd) {
    DestroyWindow(state.status_window->hwnd);
  }
}

// 隐藏悬浮状态窗：取消按钮交互状态 → 停止圆点呼吸脉动 → 隐藏窗口
auto hide(core::AppState& state) -> void {
  auto& window = *state.status_window;
  window.expires = {};
  if (window.hwnd && window.visible) {
    // 复位按钮交互
    if (auto cancelled = button::cancel(state, window.buttons, window.hwnd); !cancelled) {
      state.main_window->notice = cancelled.error();
      state.main_window->notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    }
    // 停止指示圆点呼吸动画
    if (window.indicator.node.visual) {
      auto stopped = composition::pulse(state, window.indicator.node, false);
      auto committed = stopped ? composition::commit(state) : stopped;
      if (!committed) {
        state.main_window->notice = committed.error();
        state.main_window->notice_until =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
      }
    }
    ShowWindow(window.hwnd, SW_HIDE);
  }
  window.visible = false;
}

// 刷新并对齐悬浮窗显示：检测有效生命周期 → 右下角定位 → 刷新文案与状态重绘
auto refresh(core::AppState& state, bool finished) -> void {
  auto& window = *state.status_window;
  if (!window.hwnd || window.graphics_failed) {
    return;
  }
  bool active = features::scan::running(state);
  auto now = std::chrono::steady_clock::now();
  // 扫描终结后启动 12 秒延时倒计时，到期后自动消隐
  if (finished) {
    window.expires = now + std::chrono::seconds(12);
  }
  if (!active && (!window.visible && !finished)) {
    return;
  }
  // 程序关闭或超时已过则隐藏
  if (state.main_window->closing || (!active && now >= window.expires)) {
    hide(state);
    return;
  }
  // 结束后沿用浮窗所在显示器，避免游戏恢复原位置时把结果通知移走。
  auto anchor = active || !window.visible ? state.window->game : window.hwnd;
  auto rect = position(anchor, window.dpi);
  if (!rect) {
    hide(state);
    return;
  }
  bool success = !active && state.scan->phase == features::scan::Phase::completed;
  bool failed = state.scan->phase == features::scan::Phase::failed;
  // 组装状态标题与分类名称
  std::wstring title = active    ? (state.scan->phase == features::scan::Phase::stopping
                                        ? core::i18n::wide(state, "scan.stopping_cleanup")
                                        : core::i18n::wide(state, "scan.running"))
                       : success ? core::i18n::wide(state, "scan.completed")
                       : state.scan->phase == features::scan::Phase::stopped
                           ? core::i18n::wide(state, "scan.stopped")
                           : core::i18n::wide(state, "scan.failed");
  if (active && !state.scan->progress.category.empty()) {
    title += L" · " + core::i18n::wide(state, "category." + state.scan->progress.category);
  }
  // 组装详细进度或结果概览文案
  std::wstring detail;
  if (active) {
    const auto& progress = state.scan->progress;
    if (progress.category.empty()) {
      detail = core::i18n::wide(state, features::scan::stage_message(progress.stage));
    } else {
      detail = core::i18n::wide(state, "scan.progress", progress.completed_categories,
                                progress.planned_categories, progress.category_count);
    }
  } else if (success && state.result->last_success) {
    detail = core::i18n::wide(state, "scan.results_ready",
                              state.result->last_success->categories.size());
  } else if (state.scan->phase == features::scan::Phase::stopped) {
    detail = core::i18n::wide(state, "scan.stopped_detail");
  } else {
    detail = core::i18n::wide(state, "scan.open_for_details");
  }
  // 检查状态字段是否变动以标记重绘
  auto hint = state.scan->hotkey_registered
                  ? core::i18n::wide(state, "scan.foreground_hint",
                                     std::format("F{}", state.scan->request.stop_key))
                  : core::i18n::wide(state, "scan.keep_foreground");
  if (window.title != title || window.detail != detail || window.active != active ||
      window.success != success || window.copied != state.main_window->copied ||
      window.failed != failed || window.hint != hint) {
    window.title = std::move(title);
    window.detail = std::move(detail);
    window.active = active;
    window.success = success;
    window.copied = state.main_window->copied;
    window.failed = failed;
    window.hint = std::move(hint);
    window.redraw = true;
  }
  RECT current{};
  GetWindowRect(window.hwnd, &current);
  // 跨显示器移动时由 WM_DPICHANGED 更新 DPI 并重新计算位置与尺寸。
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

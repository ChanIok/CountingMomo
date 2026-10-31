#include "ui/main_window/window.hpp"

#include "vendor/std.hpp"

#include "core/message/message.hpp"

#include "app/state.hpp"
#include "app/usecase.hpp"
#include "core/i18n/i18n.hpp"
#include "core/state/app_state.hpp"
#include "features/scan/state.hpp"
#include "ui/backdrop.hpp"
#include "ui/button.hpp"
#include "ui/composition.hpp"
#include "ui/main_window/frame.hpp"
#include "ui/main_window/layout.hpp"
#include "ui/main_window/painter.hpp"
#include "ui/main_window/state.hpp"
#include "ui/status_window/state.hpp"
#include "ui/status_window/window.hpp"
#include "ui/theme.hpp"
#include "utils/string.hpp"

namespace ui::main_window {
namespace {
auto view(core::AppState& state) -> View {
  const auto& app = *state.app;
  View result{
      .page = state.main_window->page,
      .phase = app.phase,
      .category = app.progress.category.empty()
                      ? std::wstring{}
                      : core::i18n::wide(state, "category." + app.progress.category),
      .message = core::i18n::wide(state, app.message),
      .notice = core::i18n::wide(state, app.notice),
      .theme_label =
          app.settings.theme == "system"  ? core::i18n::wide(state, "settings.theme_system")
          : app.settings.theme == "light" ? core::i18n::wide(state, "settings.theme_light")
                                          : core::i18n::wide(state, "settings.theme_dark"),
      .restore_label = app.settings.restore_window
                           ? core::i18n::wide(state, "settings.restore_on")
                           : core::i18n::wide(state, "settings.restore_off"),
      .language_label = core::i18n::current(state) == core::i18n::Language::zh_cn
                            ? core::i18n::wide(state, "settings.language_chinese")
                            : core::i18n::wide(state, "settings.language_english"),
      .category_count = app.progress.category_count,
      .has_success = app.success.has_value(),
      .diagnostic = !app.diagnostic.empty(),
      .copied = app.copied,
      .game_found = app.game && IsWindow(app.game),
  };
  if (app::running(state)) {
    result.elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::steady_clock::now() - app.started)
                         .count();
  }
  if (app.success) {
    result.warning = core::i18n::wide(state, app.success->warning);
    result.completed_seconds = app.success->elapsed_seconds;
    result.summary_count = static_cast<int>(app.success->summaries.size());
    for (const auto& summary : app.success->summaries) {
      result.total += summary.count;
      result.owned += summary.owned;
    }
  }
  return result;
}

auto failed(core::AppState& state, core::Error message) -> void {
  if (state.main_window->graphics_failed) {
    return;
  }
  state.main_window->graphics_failed = true;
  MessageBoxW(state.main_window->hwnd, core::i18n::wide(state, message).c_str(),
              core::i18n::wide(state, "error.ui.update_title").c_str(), MB_OK | MB_ICONERROR);
  app::close(state);
}

auto command(core::AppState& state, button::Action id) -> void {
  auto& window = *state.main_window;
  switch (id) {
    case button::Action::primary:
      if (app::running(state)) {
        app::stop_scan(state);
      } else if (state.app->phase == app::Phase::completed) {
        app::copy_result(state);
      } else {
        app::start_scan(state);
      }
      break;
    case button::Action::secondary:
      if (state.app->phase == app::Phase::completed) {
        app::start_scan(state);
      } else {
        app::copy_result(state);
      }
      break;
    case button::Action::settings:
      window.page = Page::settings;
      break;
    case button::Action::back:
      window.page = Page::main;
      break;
    case button::Action::details:
      if (state.app->phase == app::Phase::completed) {
        window.page = Page::details;
      }
      break;
    case button::Action::diagnostic:
      app::open_diagnostic(state);
      break;
    case button::Action::save:
      app::save_result(state);
      break;
    case button::Action::theme: {
      auto& mode = state.app->settings.theme;
      mode = mode == "system" ? "light" : mode == "light" ? "dark" : "system";
      app::save_settings(state);
      if (auto updated = update_theme(state); !updated) {
        failed(state, updated.error());
        return;
      }
      break;
    }
    case button::Action::language: {
      auto language = core::i18n::current(state) == core::i18n::Language::zh_cn
                          ? core::i18n::Language::en_us
                          : core::i18n::Language::zh_cn;
      core::i18n::set_language(state, language);
      state.app->settings.language = std::string(core::i18n::locale(language));
      app::save_settings(state);
      SetWindowTextW(window.hwnd, core::i18n::wide(state, "app.window_title").c_str());
      SetWindowTextW(state.status_window->hwnd,
                     core::i18n::wide(state, "app.status_title").c_str());
      window.redraw = true;
      window.repaint_buttons = true;
      state.status_window->redraw = true;
      state.status_window->repaint_buttons = true;
      ui::status_window::refresh(state);
      break;
    }
    case button::Action::restore:
      state.app->settings.restore_window = !state.app->settings.restore_window;
      app::save_settings(state);
      break;
    default:
      break;
  }
  if (id == button::Action::settings || id == button::Action::details ||
      id == button::Action::back) {
    window.scroll_y = 0;
    window.wheel_delta = 0;
  }
  refresh(state);
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
    state->main_window->hwnd = hwnd;
  }
  if (!state) {
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }
  auto& window = *state->main_window;
  if (auto frame_result = frame::process(hwnd, message, wparam, lparam)) {
    return *frame_result;
  }
  if (window.taskbar_message && message == window.taskbar_message) {
    window.taskbar.Reset();
    auto created = CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(window.taskbar.GetAddressOf()));
    if (SUCCEEDED(created) && SUCCEEDED(window.taskbar->HrInit())) {
      window.taskbar_progress = TBPF_NOPROGRESS;
      if (app::running(*state)) {
        window.taskbar->SetProgressState(hwnd, TBPF_INDETERMINATE);
        window.taskbar_progress = TBPF_INDETERMINATE;
      }
    } else {
      window.taskbar.Reset();
    }
    return 0;
  }
  switch (message) {
    case WM_CREATE: {
      window.taskbar_message = RegisterWindowMessageW(L"TaskbarButtonCreated");
      window.dpi = GetDpiForWindow(hwnd);
      if (!SetTimer(hwnd, 1, 250, nullptr)) {
        return -1;
      }
      return 0;
    }
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
      if (*action != button::Action::none) {
        command(*state, *action);
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
    case WM_ACTIVATE:
      if (LOWORD(wparam) == WA_INACTIVE && window.composition.target) {
        if (auto cancelled = button::cancel(*state, window.buttons, hwnd); !cancelled) {
          failed(*state, cancelled.error());
          return 0;
        }
        if (auto committed = composition::commit(*state); !committed) {
          failed(*state, committed.error());
          return 0;
        }
      }
      break;
    case WM_KEYDOWN:
      if (wparam == VK_ESCAPE) {
        if (app::running(*state)) {
          app::stop_scan(*state);
        } else if (window.page != Page::main) {
          command(*state, button::Action::back);
        }
        return 0;
      }
      break;
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
    case WM_MOUSEWHEEL: {
      window.wheel_delta += static_cast<short>(HIWORD(wparam));
      int steps = window.wheel_delta / WHEEL_DELTA;
      window.wheel_delta %= WHEEL_DELTA;
      float previous = window.scroll_y;
      window.scroll_y =
          std::clamp(previous - steps * 96.0f, 0.0f,
                     std::max(0.0f, window.layout.content_height - window.layout.body_bottom));
      if (window.scroll_y != previous) {
        window.redraw = true;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }
    case WM_SIZE:
      if (wparam != SIZE_MINIMIZED) {
        window.width = LOWORD(lparam);
        window.height = HIWORD(lparam);
        window.redraw = true;
        refresh(*state);
        layout::update(*state);
      }
      return 0;
    case WM_GETMINMAXINFO: {
      auto info = reinterpret_cast<MINMAXINFO*>(lparam);
      RECT minimum = frame::outer_rect(520, 440, window.dpi);
      info->ptMinTrackSize = {minimum.right - minimum.left, minimum.bottom - minimum.top};
      MONITORINFO monitor{sizeof(MONITORINFO)};
      if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
        info->ptMinTrackSize.x =
            std::min(info->ptMinTrackSize.x, monitor.rcWork.right - monitor.rcWork.left);
        info->ptMinTrackSize.y =
            std::min(info->ptMinTrackSize.y, monitor.rcWork.bottom - monitor.rcWork.top);
      }
      return 0;
    }
    case WM_DPICHANGED: {
      window.dpi = HIWORD(wparam);
      auto rect = reinterpret_cast<RECT*>(lparam);
      SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left,
                   rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
      frame::extend(hwnd, window.material);
      window.redraw = true;
      window.repaint_buttons = true;
      refresh(*state);
      return 0;
    }
    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
    case theme_update:
      if (auto updated = update_theme(*state); !updated) {
        failed(*state, updated.error());
      }
      return 0;
    case WM_HOTKEY:
      if (wparam == 1) {
        app::stop_scan(*state);
      }
      return 0;
    case scan_update:
      app::refresh(*state);
      return 0;
    case WM_TIMER:
      if (app::running(*state) || state->scan->worker.joinable() ||
          !state->app->notice.message.key.empty() || state->app->copied) {
        app::refresh(*state);
      } else {
        ui::status_window::refresh(*state);
      }
      return 0;
    case WM_CLOSE:
      app::close(*state);
      return 0;
    case WM_DESTROY:
      theme::shutdown(*state);
      // 先清空状态，避免释放捕获时同步的 WM_CAPTURECHANGED 再更新退场动画。
      window.buttons = {};
      if (GetCapture() == hwnd) {
        ReleaseCapture();
      }
      window.composition = {};
      KillTimer(hwnd, 1);
      UnregisterHotKey(hwnd, 1);
      ui::status_window::hide(*state);
      PostQuitMessage(0);
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
      state->scan->stop.store(true);
    }
    MessageBoxW(
        hwnd,
        utils::string::wide(
            core::i18n::text(state ? core::i18n::current(*state) : core::i18n::system_language(),
                             core::with_detail(core::error("error.ui.exception"), error.what())))
            .c_str(),
        L"CountingMomo", MB_OK | MB_ICONERROR);
    if (state && message != WM_CLOSE) {
      PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
    return 0;
  }
}
}  // namespace

auto update_theme(core::AppState& state) -> std::expected<void, core::Error> {
  if (auto refreshed = theme::refresh(state); !refreshed) {
    return refreshed;
  }
  auto colors = theme::resolve(state);
  auto& window = *state.main_window;
  window.material = backdrop::apply(window.hwnd, colors);
  frame::extend(window.hwnd, window.material);
  if (state.status_window->hwnd) {
    state.status_window->material = backdrop::apply(state.status_window->hwnd, colors, true);
  }
  window.redraw = true;
  window.repaint_buttons = true;
  state.status_window->redraw = true;
  state.status_window->repaint_buttons = true;
  refresh(state);
  ui::status_window::refresh(state);
  return {};
}

auto refresh(core::AppState& state) -> void {
  auto& window = *state.main_window;
  if (!window.hwnd) {
    return;
  }
  auto& app = *state.app;
  bool active = app::running(state);
  auto taskbar_progress = active ? TBPF_INDETERMINATE : TBPF_NOPROGRESS;
  if (window.taskbar && taskbar_progress != window.taskbar_progress) {
    window.taskbar->SetProgressState(window.hwnd, taskbar_progress);
    window.taskbar_progress = taskbar_progress;
  }
  // 最小化时不生成主面板内容；恢复后的快照包含最新进度。
  if (IsIconic(window.hwnd)) {
    return;
  }
  auto next = view(state);
  if (window.view != next) {
    bool relayout = window.view.page != next.page || window.view.phase != next.phase ||
                    window.view.warning.empty() != next.warning.empty() ||
                    window.view.summary_count != next.summary_count ||
                    window.view.language_label != next.language_label;
    window.view = std::move(next);
    window.redraw = true;
    if (relayout) {
      layout::update(state);
    }
  }
  if (window.redraw || window.repaint_buttons) {
    InvalidateRect(window.hwnd, nullptr, FALSE);
  }
}

auto create(core::AppState& state) -> std::expected<void, core::Error> {
  WNDCLASSEXW definition{sizeof(WNDCLASSEXW)};
  definition.hInstance = GetModuleHandleW(nullptr);
  definition.lpfnWndProc = window_proc;
  definition.lpszClassName = L"CountingMomoMainWindow";
  definition.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  definition.hIcon = LoadIconW(definition.hInstance, MAKEINTRESOURCEW(101));
  if (!RegisterClassExW(&definition)) {
    return std::unexpected(core::error("error.ui.register_main"));
  }
  UINT dpi = GetDpiForSystem();
  RECT rect = frame::outer_rect(560, 440, dpi);
  int width = rect.right - rect.left;
  int height = rect.bottom - rect.top;
  MONITORINFO monitor{sizeof(MONITORINFO)};
  if (GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor)) {
    width = std::min(width, static_cast<int>(monitor.rcWork.right - monitor.rcWork.left - 32));
    height = std::min(height, static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top - 32));
  }
  auto hwnd = CreateWindowExW(0, definition.lpszClassName,
                              core::i18n::wide(state, "app.window_title").c_str(),
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, width, height,
                              nullptr, nullptr, definition.hInstance, &state);
  if (!hwnd) {
    return std::unexpected(core::error("error.ui.create_main", GetLastError()));
  }
  if (auto updated = update_theme(state); !updated) {
    return updated;
  }
  SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  refresh(state);
  return {};
}

auto show(core::AppState& state) -> void {
  auto hwnd = state.main_window->hwnd;
  ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
  SetForegroundWindow(hwnd);
  refresh(state);
}
}  // namespace ui::main_window

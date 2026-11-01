#include "ui/main_window/window.hpp"

#include "vendor/std.hpp"

#include "core/i18n/i18n.hpp"
#include "core/message/message.hpp"
#include "core/state/app_state.hpp"
#include "features/result/state.hpp"
#include "features/result/usecase.hpp"
#include "features/scan/control.hpp"
#include "features/scan/state.hpp"
#include "features/scan/usecase.hpp"
#include "features/settings/state.hpp"
#include "features/settings/usecase.hpp"
#include "features/window/state.hpp"
#include "ui/backdrop.hpp"
#include "ui/button.hpp"
#include "ui/composition.hpp"
#include "ui/main_window/layout.hpp"
#include "ui/main_window/painter.hpp"
#include "ui/main_window/state.hpp"
#include "ui/status_window/state.hpp"
#include "ui/status_window/window.hpp"
#include "ui/theme.hpp"
#include "utils/string.hpp"

namespace ui::main_window {
namespace {
// 客户区 DIP 转换为含系统标题栏与调整边框的物理外框，供创建和最小尺寸共用。
auto outer_rect(int width, int height, UINT dpi) -> RECT {
  RECT rect{0, 0, MulDiv(width, dpi, 96), MulDiv(height, dpi, 96)};
  AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
  return rect;
}

auto cancel_stop_key_recording(core::AppState& state) -> void {
  auto& window = *state.main_window;
  if (!window.recording_stop_key) {
    return;
  }
  window.recording_stop_key = false;
  window.stop_key_candidate = 0;
  refresh(state);
}

// 同时接收系统按键消息，F10 才不会落入默认的菜单激活行为。
auto record_stop_key(core::AppState& state, UINT message, WPARAM key, LPARAM flags) -> bool {
  auto& window = *state.main_window;
  if (!window.recording_stop_key || window.closing) {
    return false;
  }
  bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
  bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
  if (!down && !up) {
    return false;
  }
  bool system_shortcut = (message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) &&
                         (key != VK_F10 || (flags & (LPARAM{1} << 29)));
  // 无效系统组合键不录入，但仍交给 Windows 处理 Alt+Tab、Alt+F4 等原有操作。
  if (down) {
    if (flags & (LPARAM{1} << 30)) {
      return !system_shortcut;
    }
    if (key == VK_ESCAPE || key == VK_TAB) {
      cancel_stop_key_recording(state);
      return key == VK_ESCAPE;
    }
  }
  auto modifiers = GetKeyState(VK_SHIFT) | GetKeyState(VK_CONTROL) | GetKeyState(VK_MENU) |
                   GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN);
  if (down) {
    bool valid = key >= VK_F1 && key <= VK_F11 && !(modifiers & 0x8000) && !system_shortcut;
    window.stop_key_candidate =
        valid && !window.stop_key_candidate ? static_cast<int>(key - VK_F1 + 1) : 0;
  } else if (window.stop_key_candidate && key == VK_F1 + window.stop_key_candidate - 1) {
    int candidate = std::exchange(window.stop_key_candidate, 0);
    if (!(modifiers & 0x8000) && !system_shortcut) {
      window.recording_stop_key = false;
      window.focused = button::Action::none;
      features::settings::set_stop_key(state, candidate);
    }
  }
  refresh(state);
  return !system_shortcut;
}

auto expire_feedback(core::AppState& state) -> void {
  auto& window = *state.main_window;
  if ((!window.notice.message.key.empty() || window.copied) &&
      std::chrono::steady_clock::now() >= window.notice_until) {
    window.notice = {};
    window.copied = false;
  }
}

// 从 AppState 提取当前主窗口视图快照：阶段状态、文案、耗时、统计指标与游戏存活状态
auto view(core::AppState& state) -> View {
  View result{
      .page = state.main_window->page,
      .phase = state.scan->phase,
      .category = state.scan->progress.category.empty()
                      ? std::wstring{}
                      : core::i18n::wide(state, "category." + state.scan->progress.category),
      .message = state.scan->phase == features::scan::Phase::stopped
                     ? std::wstring{}
                     : core::i18n::wide(state, state.scan->message.message),
      .notice = core::i18n::wide(state, state.main_window->notice.message),
      .theme_label = state.settings->values.theme == "system"
                         ? core::i18n::wide(state, "settings.theme_system")
                     : state.settings->values.theme == "light"
                         ? core::i18n::wide(state, "settings.theme_light")
                         : core::i18n::wide(state, "settings.theme_dark"),
      .repository_label = core::i18n::wide(state, "action.repository"),
      .language_label = core::i18n::current(state) == core::i18n::Language::zh_cn
                            ? core::i18n::wide(state, "settings.language_chinese")
                            : core::i18n::wide(state, "settings.language_english"),
      .stop_key_label = std::format(L"F{}", state.settings->values.stop_key.get()),
      .stop_key_button_label =
          state.main_window->recording_stop_key
              ? (state.main_window->stop_key_candidate
                     ? core::i18n::wide(state, "settings.stop_key_release",
                                        std::format("F{}", state.main_window->stop_key_candidate))
                     : core::i18n::wide(state, "settings.stop_key_press"))
              : core::i18n::wide(state, "settings.stop_key",
                                 std::format("F{}", state.settings->values.stop_key.get())),
      .hotkey_warning = core::i18n::wide(state, "scan.hotkey_unavailable",
                                         std::format("F{}", state.settings->values.stop_key.get())),
      .hotkey_available = state.scan->hotkey_available,
      .focused = state.main_window->focused,
      .category_count = state.scan->progress.category_count,
      .has_success = state.result->last_success.has_value(),
      .diagnostic = !state.scan->diagnostic.empty(),
      .copied = state.main_window->copied,
      .game_found = state.window->game && IsWindow(state.window->game),
  };
  // 运行中动态计算已流逝时长
  if (features::scan::running(state)) {
    result.elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::steady_clock::now() - state.scan->started)
                         .count();
  }
  // 汇总分类统计指标
  if (state.result->last_success) {
    result.warning = core::i18n::wide(state, state.result->last_success->warning.message);
    result.completed_seconds = state.result->last_success->elapsed_seconds;
    result.summary_count = static_cast<int>(state.result->last_success->summaries.size());
    for (const auto& summary : state.result->last_success->summaries) {
      result.total += summary.count;
      result.owned += summary.owned;
    }
  }
  return result;
}

// 弹出图形渲染致命错误弹窗并触发退出流程
auto failed(core::AppState& state, core::Error message) -> void {
  if (state.main_window->graphics_failed) {
    return;
  }
  state.main_window->graphics_failed = true;
  MessageBoxW(state.main_window->hwnd, core::i18n::wide(state, message).c_str(),
              core::i18n::wide(state, "error.ui.update_title").c_str(), MB_OK | MB_ICONERROR);
  ui::main_window::close(state);
}

// 响应主窗口按钮点击动作：启停扫描、切换视图、持久化设置与导出结果
auto command(core::AppState& state, button::Action id) -> void {
  auto& window = *state.main_window;
  if (window.closing) {
    return;
  }
  if (id != button::Action::stop_key) {
    cancel_stop_key_recording(state);
  }
  switch (id) {
    case button::Action::primary:
      // 运行中终止，完成态复制，其余阶段启动扫描
      if (features::scan::running(state)) {
        features::scan::stop(state);
      } else if (state.scan->phase == features::scan::Phase::completed) {
        features::result::copy(state);
      } else {
        features::scan::start(state);
      }
      break;
    case button::Action::secondary:
      // 完成态重新扫描，其它状态复制上次结果
      if (state.scan->phase == features::scan::Phase::completed) {
        features::scan::start(state);
      } else {
        features::result::copy(state);
      }
      break;
    case button::Action::settings:
      // 进入设置页
      window.page = Page::settings;
      break;
    case button::Action::back:
      // 返回主界面
      window.page = Page::main;
      break;
    case button::Action::details:
      // 查看分类统计明细
      if (state.scan->phase == features::scan::Phase::completed) {
        window.page = Page::details;
      }
      break;
    case button::Action::diagnostic:
      // 打开诊断目录
      features::scan::open_diagnostic(state);
      break;
    case button::Action::save:
      // 保存结果文件
      features::result::save(state);
      break;
    case button::Action::theme: {
      // 循环切换跟随系统 / 浅色 / 深色主题
      if (auto updated = features::settings::cycle_theme(state); !updated) {
        failed(state, updated.error());
        return;
      }
      break;
    }
    case button::Action::language: {
      // 切换中英文
      features::settings::toggle_language(state);
      break;
    }
    case button::Action::repository:
      // 用系统默认浏览器打开项目仓库
      features::settings::open_repository(state);
      break;
    case button::Action::stop_key: {
      if (features::scan::running(state)) {
        break;
      }
      window.recording_stop_key = true;
      window.stop_key_candidate = 0;
      break;
    }
    default:
      break;
  }
  // 切页时重置滚动位置
  if (id == button::Action::settings || id == button::Action::details ||
      id == button::Action::back) {
    window.focused = button::Action::none;
    window.scroll_y = 0;
    window.wheel_delta = 0;
  }
  refresh(state);
}

// 执行主窗口重绘并处理显卡设备丢失后的自动恢复与二次尝试
auto paint(core::AppState& state) -> std::expected<void, core::Error> {
  auto painted = painter::paint(state);
  // 绘制失败时尝试触发显卡驱动丢失恢复
  if (!painted) {
    auto recovered = composition::recover(state);
    if (!recovered) {
      return std::unexpected(recovered.error());
    }
    // 恢复成功后二次执行重绘
    if (*recovered) {
      return painter::paint(state);
    }
  }
  return painted;
}

// 主窗口处理客户区交互与生命周期；原生标题栏及系统按钮消息交由 DefWindowProcW。
auto procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT {
  auto state = reinterpret_cast<core::AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  // 窗口初次创建时绑定 AppState 实例指针到 GWLP_USERDATA
  if (message == WM_NCCREATE) {
    state = static_cast<core::AppState*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    state->main_window->hwnd = hwnd;
  }
  if (!state) {
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }
  auto& window = *state->main_window;
  // 录入只属于当前设置页；鼠标操作、失焦和退出都不能提交尚未松开的候选键。
  bool cancel_recording =
      message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN ||
      message == WM_NCLBUTTONDOWN || message == WM_NCRBUTTONDOWN || message == WM_KILLFOCUS ||
      message == WM_CANCELMODE || message == WM_ENTERMENULOOP || message == WM_CLOSE ||
      (message == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE);
  if (cancel_recording) {
    cancel_stop_key_recording(*state);
  }
  if (record_stop_key(*state, message, wparam, lparam)) {
    return 0;
  }
  // Windows 任务栏按钮创建广播：初始化 ITaskbarList3 接口
  if (window.taskbar_message && message == window.taskbar_message) {
    window.taskbar.Reset();
    auto created = CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(window.taskbar.GetAddressOf()));
    if (SUCCEEDED(created) && SUCCEEDED(window.taskbar->HrInit())) {
      window.taskbar_progress = TBPF_NOPROGRESS;
      if (features::scan::running(*state)) {
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
      // 启动 250ms 定时器驱动后台同步与悬浮窗轮询
      if (!SetTimer(hwnd, 1, 250, nullptr)) {
        return -1;
      }
      return 0;
    }
    case WM_MOUSEMOVE:
    case WM_MOUSELEAVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP: {
      if (window.closing) {
        return 0;
      }
      if (message == WM_LBUTTONDOWN && window.focused != button::Action::none) {
        window.focused = button::Action::none;
        refresh(*state);
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
      // 驱动通用按钮手势状态机
      auto action = button::pointer(*state, window.buttons, hwnd, message, lparam, window.dpi);
      if (!action) {
        failed(*state, action.error());
        return 0;
      }
      // 按钮交互状态变化时提交视觉树动效
      if (hovered != window.buttons.hovered || pressed != window.buttons.pressed) {
        if (auto committed = composition::commit(*state); !committed) {
          failed(*state, committed.error());
          return 0;
        }
      }
      // 触发按钮关联的业务命令
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
      if (window.closing) {
        return 0;
      }
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
      if (!window.closing && LOWORD(wparam) == WA_INACTIVE && window.composition.target) {
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
      // 录入取消后，仍按住 Esc/激活键不能再触发返回或重新开始录入。
      if ((wparam == VK_RETURN || wparam == VK_SPACE || wparam == VK_ESCAPE) &&
          (lparam & (LPARAM{1} << 30))) {
        return 0;
      }
      if (window.closing) {
        return 0;
      }
      if (window.redraw || window.repaint_buttons) {
        if (auto painted = paint(*state); !painted) {
          failed(*state, painted.error());
          return 0;
        }
      }
      if (wparam == VK_TAB) {
        std::vector<const button::Spec*> available;
        for (const auto& item : window.buttons.buttons) {
          if (item.spec.enabled) {
            available.push_back(&item.spec);
          }
        }
        if (!available.empty()) {
          int index = -1;
          for (int i = 0; i < static_cast<int>(available.size()); ++i) {
            if (available[i]->action == window.focused) {
              index = i;
            }
          }
          bool backwards = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
          int count = static_cast<int>(available.size());
          index = index < 0 ? (backwards ? count - 1 : 0)
                            : (index + (backwards ? count - 1 : 1)) % count;
          const auto& target = *available[index];
          window.focused = target.action;
          if (target.action == button::Action::theme || target.action == button::Action::language ||
              target.action == button::Action::stop_key ||
              target.action == button::Action::primary) {
            if (target.bounds.top < layout::header_bottom) {
              window.scroll_y =
                  std::max(0.0f, window.scroll_y + target.bounds.top - layout::header_bottom);
            }
            if (target.bounds.bottom > window.layout.body_bottom) {
              window.scroll_y += target.bounds.bottom - window.layout.body_bottom;
            }
            window.redraw = true;
          }
          refresh(*state);
        }
        return 0;
      }
      if ((wparam == VK_RETURN || wparam == VK_SPACE) && window.focused != button::Action::none) {
        if (std::ranges::any_of(window.buttons.buttons, [&](const button::Button& item) {
              return item.spec.action == window.focused && item.spec.enabled;
            })) {
          command(*state, window.focused);
        }
        return 0;
      }
      // ESC 键中断扫描或退回主页
      if (wparam == VK_ESCAPE) {
        if (features::scan::running(*state)) {
          features::scan::stop(*state);
        } else if (window.page != Page::main) {
          command(*state, button::Action::back);
        }
        return 0;
      }
      break;
    case WM_PAINT: {
      PAINTSTRUCT native_paint{};
      BeginPaint(hwnd, &native_paint);
      // 将窗口 GDI 底层清理为全黑以配合 DComp 透明混合
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
      // 累计滚轮步数并计算内容纵向滚动偏移
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
        // 更新窗口像素大小并刷新排版
        window.width = LOWORD(lparam);
        window.height = HIWORD(lparam);
        window.redraw = true;
        refresh(*state);
        layout::update(*state);
      }
      return 0;
    case WM_GETMINMAXINFO: {
      // 限制主窗口最小尺寸
      auto info = reinterpret_cast<MINMAXINFO*>(lparam);
      RECT minimum = outer_rect(520, 408, window.dpi);
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
      // 系统建议的外框已包含原生标题栏；客户区布局按新 DPI 重新度量。
      window.dpi = HIWORD(wparam);
      auto rect = reinterpret_cast<RECT*>(lparam);
      SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left,
                   rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
      layout::update(*state);
      window.redraw = true;
      window.repaint_buttons = true;
      refresh(*state);
      return 0;
    }
    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
    case WM_DWMCOMPOSITIONCHANGED:
    case theme_update:
      // 系统主题色发生变动
      if (auto updated = update_theme(*state); !updated) {
        failed(*state, updated.error());
      }
      break;
    case WM_HOTKEY:
      if (wparam == features::scan::stop_hotkey_id && state->scan->hotkey_registered &&
          HIWORD(lparam) == VK_F1 + state->scan->request.stop_key - 1) {
        features::scan::stop(*state);
      }
      return 0;
    case activate_instance:
      show(*state);
      return 0;
    case scan_update:
      // 扫描工作线程状态推进同步
      features::scan::consume_updates(*state);
      return 0;
    case WM_TIMER:
      expire_feedback(*state);
      // 定时轮询扫描状态与浮窗刷新
      if (features::scan::running(*state) || state->scan->worker.joinable()) {
        features::scan::consume_updates(*state);
      } else {
        ui::status_window::refresh(*state);
        refresh(*state);
      }
      return 0;
    case WM_CLOSE:
      ui::main_window::close(*state);
      return 0;
    case WM_DESTROY:
      // 先清空状态，避免释放捕获时同步的 WM_CAPTURECHANGED 再更新退场动画。
      window.buttons = {};
      if (GetCapture() == hwnd) {
        ReleaseCapture();
      }
      window.composition = {};
      return 0;
    case WM_NCDESTROY:
      window.hwnd = nullptr;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      break;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

// 顶层 Win32 窗口过程包装：捕获运行时异常以安全退出并弹窗报警
auto CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT {
  try {
    return procedure(hwnd, message, wparam, lparam);
  } catch (const std::exception& error) {
    auto state = reinterpret_cast<core::AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (state) {
      features::scan::request_stop(*state);
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

// 刷新全局主题配色方案：更新背景材质、边框扩展并标记全量重绘
auto update_theme(core::AppState& state) -> std::expected<void, core::Error> {
  if (auto refreshed = theme::refresh(state); !refreshed) {
    return refreshed;
  }
  auto colors = theme::resolve(state);
  auto& window = *state.main_window;
  // 应用主窗口 Mica 材质与 DWM 扩展
  window.material = backdrop::apply(window.hwnd, colors);
  // 应用状态浮窗 Acrylic 材质
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

// 检查并同步主窗口视图快照：更新任务栏进度、重算布局差异并触发重绘
auto refresh(core::AppState& state) -> void {
  auto& window = *state.main_window;
  if (!window.hwnd) {
    return;
  }
  bool active = features::scan::running(state);
  // 同步 Windows 任务栏闪烁/进度条状态
  auto taskbar_progress = active ? TBPF_INDETERMINATE : TBPF_NOPROGRESS;
  if (window.taskbar && taskbar_progress != window.taskbar_progress) {
    window.taskbar->SetProgressState(window.hwnd, taskbar_progress);
    window.taskbar_progress = taskbar_progress;
  }
  // 最小化时不生成主面板内容；恢复后的快照包含最新进度。
  if (IsIconic(window.hwnd)) {
    return;
  }
  // 提取最新视图快照并比对是否需要重新排版
  auto next = view(state);
  if (window.view != next) {
    bool relayout = window.view.page != next.page || window.view.phase != next.phase ||
                    window.view.hotkey_available != next.hotkey_available ||
                    window.view.has_success != next.has_success ||
                    window.view.diagnostic != next.diagnostic ||
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

// 创建并初始化主窗口：注册窗口类 → 测算初始居中尺寸 → 创建窗口句柄并应用主题
auto create(core::AppState& state) -> std::expected<void, core::Error> {
  // 注册主窗口类
  WNDCLASSEXW definition{sizeof(WNDCLASSEXW)};
  definition.hInstance = GetModuleHandleW(nullptr);
  definition.lpfnWndProc = window_proc;
  definition.lpszClassName = class_name;
  definition.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  definition.hIcon = LoadIconW(definition.hInstance, MAKEINTRESOURCEW(101));
  if (!RegisterClassExW(&definition)) {
    return std::unexpected(core::error("error.ui.register_main"));
  }
  // 根据显示器 DPI 计算适宜的初始外框矩形尺寸
  UINT dpi = GetDpiForSystem();
  RECT rect = outer_rect(560, 408, dpi);
  int width = rect.right - rect.left;
  int height = rect.bottom - rect.top;
  MONITORINFO monitor{sizeof(MONITORINFO)};
  if (GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor)) {
    width = std::min(width, static_cast<int>(monitor.rcWork.right - monitor.rcWork.left - 32));
    height = std::min(height, static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top - 32));
  }
  // 创建 Win32 顶层窗口
  auto hwnd = CreateWindowExW(0, definition.lpszClassName,
                              core::i18n::wide(state, "app.window_title").c_str(),
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, width, height,
                              nullptr, nullptr, definition.hInstance, &state);
  if (!hwnd) {
    return std::unexpected(core::error("error.ui.create_main", GetLastError()));
  }
  // 仅额外放行无参数的唤起消息，供普通权限的后续启动进程使用。
  if (!ChangeWindowMessageFilterEx(hwnd, activate_instance, MSGFLT_ALLOW, nullptr)) {
    auto error = GetLastError();
    return std::unexpected(
        core::with_detail(core::error("error.ui.allow_activation"),
                          std::format("ChangeWindowMessageFilterEx: {}", error)));
  }
  // 初始化窗口材质与深浅色主题
  if (auto updated = update_theme(state); !updated) {
    return updated;
  }
  refresh(state);
  return {};
}

auto activate_existing() -> std::expected<void, core::Error> {
  auto hwnd = FindWindowW(class_name, nullptr);
  // 已有进程可能仍在初始化或收尾，本次启动不等待它创建窗口。
  if (!hwnd) {
    return {};
  }
  DWORD process{};
  GetWindowThreadProcessId(hwnd, &process);
  AllowSetForegroundWindow(process);
  if (!PostMessageW(hwnd, activate_instance, 0, 0)) {
    auto error = GetLastError();
    return std::unexpected(core::with_detail(core::error("error.startup.activate_existing"),
                                             std::format("PostMessageW: {}", error)));
  }
  return {};
}

// 恢复并激活主窗口前台呈现
auto show(core::AppState& state) -> void {
  auto hwnd = state.main_window->hwnd;
  ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
  SetForegroundWindow(hwnd);
  refresh(state);
}

auto update_language(core::AppState& state) -> void {
  auto& window = *state.main_window;
  SetWindowTextW(window.hwnd, core::i18n::wide(state, "app.window_title").c_str());
  SetWindowTextW(state.status_window->hwnd, core::i18n::wide(state, "app.status_title").c_str());
  window.redraw = true;
  window.repaint_buttons = true;
  state.status_window->redraw = true;
  state.status_window->repaint_buttons = true;
  ui::status_window::refresh(state);
  refresh(state);
}

auto notify(core::AppState& state, core::Error message) -> void {
  // 覆盖复制成功的瞬态反馈
  state.main_window->copied = false;
  state.main_window->notice = std::move(message);
  // 通知固定展示 6 秒后自动消隐
  state.main_window->notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(6);
  ui::main_window::refresh(state);
}

auto close(core::AppState& state) -> void {
  cancel_stop_key_recording(state);
  state.main_window->closing = true;
  // 保留消息循环消费终态；线程回收后再次调用 close 才请求退出。
  if (features::scan::running(state)) {
    features::scan::stop(state);
    SetWindowTextW(state.main_window->hwnd, core::i18n::wide(state, "app.closing").c_str());
  } else {
    PostQuitMessage(0);
  }
}

auto prepare_shutdown(core::AppState& state) -> void {
  auto& window = *state.main_window;
  window.closing = true;
  if (window.hwnd) {
    KillTimer(window.hwnd, 1);
    features::scan::release_hotkey(state);
  }
}

auto destroy(core::AppState& state) -> void {
  if (state.main_window->hwnd) {
    DestroyWindow(state.main_window->hwnd);
  }
}
}  // namespace ui::main_window

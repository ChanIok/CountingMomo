#include "features/scan/usecase.hpp"

#include "vendor/std.hpp"

#include "vendor/windows.hpp"
#include "vendor/windows/shellapi.hpp"

#include "core/message/message.hpp"
#include "core/state/app_state.hpp"
#include "features/result/state.hpp"
#include "features/scan/control.hpp"
#include "features/scan/job.hpp"
#include "features/scan/state.hpp"
#include "features/settings/state.hpp"
#include "features/window/state.hpp"
#include "features/window/window.hpp"
#include "ui/main_window/state.hpp"
#include "ui/main_window/window.hpp"
#include "ui/status_window/state.hpp"
#include "ui/status_window/window.hpp"
#include "utils/file.hpp"

namespace features::scan {

auto running(const core::AppState& state) -> bool {
  return state.scan->phase == Phase::running || state.scan->phase == Phase::stopping;
}

auto start(core::AppState& state) -> void {
  // 任务已在运行或停止中则忽略重复请求
  if (running(state) || state.main_window->closing) {
    return;
  }
  state.scan->diagnostic.clear();
  auto root = utils::file::executable_directory();
  if (!root) {
    state.scan->phase = Phase::failed;
    state.scan->message = root.error();
    ui::main_window::refresh(state);
    return;
  }
  // 再次精确定位目标游戏主窗口
  auto game = features::window::find(nullptr, features::window::game_executable);
  if (!game) {
    state.scan->phase = Phase::failed;
    state.scan->message = game.error();
    ui::main_window::refresh(state);
    return;
  }
  state.window->game = *game;
  // 快捷停止不可用不影响扫描，失焦始终可以终止任务。
  register_hotkey(state);
  // 诊断根目录仅传入任务；异常收尾时才创建本次目录。
  features::scan::Request request;
  request.hwnd = state.window->game;
  request.diagnostic_root = *root / "data/diagnostics";
  request.stop_key = state.settings->values.stop_key.get();
  request.hotkey_enabled = state.scan->hotkey_registered;
  // 跨线程唤醒回调：向主窗口投递自定义扫描进度消息
  state.scan->wake = [hwnd = state.main_window->hwnd] {
    PostMessageW(hwnd, ui::main_window::scan_update, 0, 0);
  };
  // 初始化运行期状态
  state.scan->phase = Phase::running;
  state.scan->progress = {};
  state.scan->message = {};
  state.main_window->notice = {};
  state.main_window->copied = false;
  state.scan->started = std::chrono::steady_clock::now();
  // 切回主界面并重置滚动位置
  state.main_window->page = ui::main_window::Page::main;
  state.main_window->scroll_y = 0;
  state.main_window->wheel_delta = 0;
  // 若悬浮窗此前发生过设备丢失，在此处重建资源状态
  if (state.status_window->graphics_failed) {
    state.status_window->buttons = {};
    state.status_window->indicator = {};
    state.status_window->composition = {};
    state.status_window->redraw = true;
    state.status_window->repaint_buttons = true;
  }
  state.status_window->graphics_failed = false;
  // 启动后台扫描工作线程
  if (auto started = job::start(state, std::move(request)); !started) {
    release_hotkey(state);
    state.scan->phase = Phase::failed;
    state.scan->message = started.error();
  } else {
    // 成功启动后主窗口自动最小化，交由状态悬浮窗呈现进度
    ShowWindow(state.main_window->hwnd, SW_MINIMIZE);
  }
  ui::main_window::refresh(state);
}

auto stop(core::AppState& state) -> void {
  if (!running(state)) {
    return;
  }
  // 向扫描核心注入退出请求信号
  request_stop(state);
  state.scan->phase = Phase::stopping;
  ui::main_window::refresh(state);
  ui::status_window::refresh(state);
}

auto consume_updates(core::AppState& state) -> void {
  // 检查后台线程是否已完成并交付结果
  auto result = job::consume(state);
  if (result) {
    release_hotkey(state);
    state.scan->diagnostic = result->diagnostic;
    state.scan->message = result->message;
    // 成功、失败或停止都可能需要用户处理窗口恢复等收尾事项。
    if (!result->warning.message.key.empty()) {
      state.main_window->notice = result->warning;
      state.main_window->notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    }
    if (result->status == Phase::completed) {
      state.scan->phase = Phase::completed;
      state.result->last_success = std::move(*result);
    } else {
      state.scan->phase = result->status == Phase::stopped ? Phase::stopped : Phase::failed;
    }
    // 已经消费终态并回收线程，现在可以退出消息循环，交给应用 shutdown 销毁资源。
    if (state.main_window->closing) {
      ui::main_window::close(state);
      return;
    }
    // 扫描完成并收回悬浮窗
    ui::status_window::refresh(state, true);
  } else if (running(state)) {
    // 仍在进行中，刷新悬浮窗进度显示
    ui::status_window::refresh(state);
  }
  ui::main_window::refresh(state);
}

auto open_diagnostic(core::AppState& state) -> void {
  if (running(state)) {
    return;
  }
  std::error_code error;
  // 确保诊断目录存在且为有效文件夹
  if (state.scan->diagnostic.empty() ||
      !std::filesystem::is_directory(state.scan->diagnostic, error)) {
    ui::main_window::notify(state, core::error("error.app.no_diagnostics"));
    return;
  }
  // 通过 ShellExecuteW 调起资源管理器窗口
  auto result = ShellExecuteW(state.main_window->hwnd, L"open", state.scan->diagnostic.c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(result) <= 32) {
    ui::main_window::notify(state, core::error("error.app.open_diagnostics"));
  }
}

}  // namespace features::scan

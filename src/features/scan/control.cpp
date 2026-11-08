#include "features/scan/control.hpp"

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

#include "core/state/app_state.hpp"
#include "features/scan/capture/capture.hpp"
#include "features/scan/state.hpp"
#include "features/settings/state.hpp"
#include "ui/main_window/state.hpp"

namespace features::scan {

auto register_hotkey(core::AppState& state) -> void {
  auto& scan = *state.scan;
  if (scan.hotkey_registered) {
    return;
  }
  scan.hotkey_registered =
      RegisterHotKey(state.main_window->hwnd, stop_hotkey_id, MOD_NOREPEAT,
                     VK_F1 + state.settings->values.stop_key.get() - 1) != FALSE;
  scan.hotkey_available = scan.hotkey_registered;
}

auto release_hotkey(core::AppState& state) -> void {
  if (state.scan->hotkey_registered) {
    UnregisterHotKey(state.main_window->hwnd, stop_hotkey_id);
    state.scan->hotkey_registered = false;
  }
}

auto check_hotkey(core::AppState& state) -> void {
  if (state.scan->worker.joinable()) {
    return;
  }
  register_hotkey(state);
  release_hotkey(state);
}

auto request_stop(core::AppState& state) -> void {
  state.scan->stop.store(true);
  capture::wake(state);
}

auto stop_requested(core::AppState& state) -> bool {
  const auto& request = state.scan->request;
  if (request.hotkey_enabled && (GetAsyncKeyState(VK_F1 + request.stop_key - 1) & 0x8000)) {
    // 与无修饰键的全局注册一致，组合键不应被工作线程误判为停止键。
    auto modifiers = GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_CONTROL) |
                     GetAsyncKeyState(VK_MENU) | GetAsyncKeyState(VK_LWIN) |
                     GetAsyncKeyState(VK_RWIN);
    if (!(modifiers & 0x8000)) {
      state.scan->stop.store(true);
    }
  }
  // 返回原子停止状态
  return state.scan->stop.load();
}
}  // namespace features::scan

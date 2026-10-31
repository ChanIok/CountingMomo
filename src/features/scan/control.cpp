#include "features/scan/control.hpp"

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

#include "core/state/app_state.hpp"
#include "features/capture/capture.hpp"
#include "features/scan/state.hpp"

namespace features::scan {

auto request_stop(core::AppState& state) -> void {
  state.scan->stop.store(true);
  capture::wake(state);
}

auto stop_requested(core::AppState& state) -> bool {
  // 检查热键 F8 是否按下
  if (GetAsyncKeyState(VK_F8) & 0x8000) {
    state.scan->stop.store(true);
  }
  // 返回原子停止状态
  return state.scan->stop.load();
}
}  // namespace features::scan

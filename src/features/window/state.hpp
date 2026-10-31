#pragma once

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

namespace features::window {
struct SavedWindow {
  HWND hwnd{};
  DWORD pid{};
  LONG_PTR style{};
  LONG_PTR ex_style{};
  WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
};

struct State {
  HWND game{};  // UI 线程更新，任务启动前复制进扫描请求。
  std::optional<SavedWindow> original;
};
}  // namespace features::window

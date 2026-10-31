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
  bool restore_enabled{};  // 仅扫描线程访问。
  std::optional<SavedWindow> original;
};
}  // namespace features::window

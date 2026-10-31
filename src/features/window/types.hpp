#pragma once

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

namespace features::window {

struct Geometry {
  int width{};
  int height{};
  POINT origin{};  // 客户区左上角的屏幕坐标。
};

struct Target {
  HWND hwnd{};
  DWORD pid{};
  int width{};
  int height{};
};
}  // namespace features::window

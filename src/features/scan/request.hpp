#pragma once

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

namespace features::scan {

struct Request {
  std::wstring exe_name{L"X6Game-Win64-Shipping.exe"};
  HWND hwnd{};
  std::filesystem::path layout;
  std::filesystem::path categories;
  std::filesystem::path output;
  int overlap_rows{4};  // 默认最多 3 行文字锚点，另留 1 行余量。
  int max_scrolls{1000};
  double frame_timeout{8.0};
  double stable_seconds{0.25};
  bool restore_window{true};
};

}  // namespace features::scan

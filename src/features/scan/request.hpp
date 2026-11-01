#pragma once

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

namespace features::scan {

struct Request {
  HWND hwnd{};
  std::filesystem::path diagnostic_root;
  int stop_key{10};
  bool hotkey_enabled{};
};

}  // namespace features::scan

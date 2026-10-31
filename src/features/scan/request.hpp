#pragma once

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

namespace features::scan {

struct Request {
  HWND hwnd{};
  std::filesystem::path diagnostic_root;
};

}  // namespace features::scan

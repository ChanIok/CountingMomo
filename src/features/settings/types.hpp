#pragma once

#include "vendor/std.hpp"

namespace features::settings {

struct Settings {
  std::string theme{"system"};
  std::optional<std::string> language;  // 未手动选择时按系统显示语言决定默认值。
};
}  // namespace features::settings

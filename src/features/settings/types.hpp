#pragma once

#include "vendor/std.hpp"

#include "vendor/rfl.hpp"

namespace features::settings {

struct Settings {
  std::string theme{"system"};
  std::optional<std::string> language;  // 未手动选择时按系统显示语言决定默认值。
  rfl::DefaultVal<int> stop_key{10};    // 功能键编号 F1～F11；字段缺省时使用 F10。
};
}  // namespace features::settings

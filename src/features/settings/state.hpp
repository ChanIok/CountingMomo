#pragma once

#include "vendor/std.hpp"

#include "features/settings/types.hpp"

namespace features::settings {

struct State {
  Settings values;
  std::filesystem::path path;
};
}  // namespace features::settings

#pragma once

#include "vendor/std.hpp"

namespace core {
struct AppState;
}

namespace features::result {
auto copy(core::AppState& state) -> void;
auto save(core::AppState& state) -> void;
}  // namespace features::result

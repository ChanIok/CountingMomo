#pragma once

#include "vendor/std.hpp"

#include "core/message/types.hpp"
#include "core/startup/types.hpp"

namespace core {
struct AppState;
}

namespace core::startup {

auto prepare(AppState& state, const wchar_t* arguments, int show_command)
    -> std::expected<Action, Error>;
auto release_instance(AppState& state) -> void;

}  // namespace core::startup

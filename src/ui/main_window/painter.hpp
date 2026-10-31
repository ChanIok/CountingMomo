#pragma once
#include "vendor/std.hpp"

#include "core/message/message.hpp"

namespace core {
struct AppState;
}

namespace ui::main_window::painter {
auto paint(core::AppState& state) -> std::expected<void, core::Error>;
}

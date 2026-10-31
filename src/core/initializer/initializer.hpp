#pragma once

#include "vendor/std.hpp"

#include "core/message/types.hpp"
#include "core/startup/types.hpp"

namespace core {
struct AppState;
}

namespace core::initializer {
// 只有 run 才进入消息循环；exit 或失败也由应用入口统一调用 shutdown_application 收尾。
auto initialize_application(AppState& state, const wchar_t* arguments, int show_command)
    -> std::expected<startup::Action, Error>;
}  // namespace core::initializer

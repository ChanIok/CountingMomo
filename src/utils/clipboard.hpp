#pragma once

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

#include "core/message/message.hpp"

namespace utils::clipboard {
// 成功后系统取得内存所有权；失败保留现有剪贴板（无法设置数据的系统错误除外）。
auto write_text(HWND owner, std::string_view utf8) -> std::expected<void, core::Error>;
}  // namespace utils::clipboard

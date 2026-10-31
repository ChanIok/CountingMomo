#pragma once

#include "vendor/std.hpp"

#include "core/message/message.hpp"
#include "features/result/types.hpp"

namespace features::result {

auto serialize(const Categories& categories) -> std::expected<std::string, core::Error>;

// 已有结果先写入同目录临时文件，再替换，避免中断破坏已有结果文件。
auto write(const std::filesystem::path& path, const Categories& categories)
    -> std::expected<void, core::Error>;
}  // namespace features::result

#pragma once

#include "vendor/std.hpp"

#include "core/message/message.hpp"

namespace utils::file {

auto read(const std::filesystem::path& path) -> std::expected<std::string, core::Error>;
auto write(const std::filesystem::path& path, std::string_view text)
    -> std::expected<void, core::Error>;
auto executable_directory() -> std::filesystem::path;
auto timestamp() -> std::string;
}  // namespace utils::file

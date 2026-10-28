#pragma once

#include "vendor/std.hpp"

namespace utils::file {
auto read(const std::filesystem::path& path) -> std::expected<std::string, std::string>;
auto write(const std::filesystem::path& path, std::string_view text)
    -> std::expected<void, std::string>;
auto executable_directory() -> std::filesystem::path;
auto timestamp() -> std::string;
}  // namespace utils::file

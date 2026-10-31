#pragma once

#include "vendor/std.hpp"

namespace utils::string {

auto utf8(std::wstring_view text) -> std::string;
auto wide(std::string_view text) -> std::wstring;
auto path_utf8(const std::filesystem::path& path) -> std::string;
}  // namespace utils::string

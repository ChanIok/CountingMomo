#pragma once

#include "vendor/std.hpp"

namespace features::recognition {
struct LibraryName {
  std::string text;
  std::vector<int> glyphs;
};
struct NameLibrary {
  std::vector<LibraryName> names;
  std::vector<char32_t> characters;
  std::vector<std::string> ignored_names;
};
auto load_names(const std::filesystem::path& path) -> std::expected<NameLibrary, std::string>;
}  // namespace features::recognition

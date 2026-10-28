#include "features/recognition/name_library.hpp"

#include "vendor/std.hpp"

#include "utils/file.hpp"
#include "utils/string.hpp"

namespace features::recognition {
auto load_names(const std::filesystem::path& path) -> std::expected<NameLibrary, std::string> {
  auto data = utils::file::read(path);
  if (!data) return std::unexpected(data.error() + "；请使用 --names 指定名称 TXT。");
  std::string_view remaining(*data);
  if (remaining.starts_with("\xef\xbb\xbf")) remaining.remove_prefix(3);
  NameLibrary library;
  std::vector<std::u32string> decoded;
  std::unordered_set<std::string> seen;
  int line_number = 0;
  while (!remaining.empty()) {
    ++line_number;
    auto end = remaining.find('\n');
    auto line = remaining.substr(0, end);
    remaining.remove_prefix(end == std::string_view::npos ? remaining.size() : end + 1);
    auto first = line.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) continue;
    line = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
    // 已确认不在扫描范围内；保留TXT原文，待源数据统一清洗。
    if (line == "遇上喵喵雨・星彩") {
      if (seen.emplace(line).second) library.ignored_names.emplace_back(line);
      continue;
    }
    auto wide = utils::string::wide(line);
    if (wide.empty())
      return std::unexpected(std::format("名称库第{}行不是有效UTF-8。", line_number));
    std::u32string codepoints;
    for (std::size_t i = 0; i < wide.size(); ++i) {
      char32_t cp = wide[i];
      if (cp >= 0xd800 && cp <= 0xdbff) {
        cp = 0x10000 + ((cp - 0xd800) << 10) + (wide[++i] - 0xdc00);
      }
      if (cp < 32 || cp == 127 || cp == U'•' || cp == 0xfeff)
        return std::unexpected(std::format("名称库第{}行包含不支持的控制字符或•。", line_number));
      codepoints.push_back(cp);
    }
    if (!seen.emplace(line).second) continue;
    library.names.push_back({std::string(line), {}});
    library.characters.insert(library.characters.end(), codepoints.begin(), codepoints.end());
    decoded.push_back(std::move(codepoints));
  }
  if (library.names.empty()) return std::unexpected("名称库为空。");
  std::ranges::sort(library.characters);
  auto unique = std::ranges::unique(library.characters);
  library.characters.erase(unique.begin(), unique.end());
  for (std::size_t i = 0; i < decoded.size(); ++i) {
    for (char32_t cp : decoded[i])
      library.names[i].glyphs.push_back(static_cast<int>(
          std::ranges::lower_bound(library.characters, cp) - library.characters.begin()));
  }
  return library;
}
}  // namespace features::recognition

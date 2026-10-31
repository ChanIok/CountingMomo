#include "utils/string.hpp"

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

namespace utils::string {

auto utf8(std::wstring_view text) -> std::string {
  if (text.empty()) {
    return {};
  }
  int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                 nullptr, nullptr);
  std::string result(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size,
                      nullptr, nullptr);
  return result;
}

auto wide(std::string_view text) -> std::wstring {
  if (text.empty()) {
    return {};
  }
  int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                 static_cast<int>(text.size()), nullptr, 0);
  std::wstring result(size, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                      result.data(), size);
  return result;
}

auto path_utf8(const std::filesystem::path& path) -> std::string { return utf8(path.wstring()); }
}  // namespace utils::string

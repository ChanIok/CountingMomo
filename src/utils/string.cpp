#include "utils/string.hpp"

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

namespace utils::string {

// 将 UTF-16 宽字符串转换为 UTF-8 多字节字符串
auto utf8(std::wstring_view text) -> std::string {
  if (text.empty()) {
    return {};
  }
  // 计算 UTF-8 编码所需的目标缓冲区长度并转换
  int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                 nullptr, nullptr);
  std::string result(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size,
                      nullptr, nullptr);
  return result;
}

// 将 UTF-8 多字节字符串转换为 UTF-16 宽字符串
auto wide(std::string_view text) -> std::wstring {
  if (text.empty()) {
    return {};
  }
  // 计算 UTF-16 编码所需的目标缓冲区长度并转换
  int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                 static_cast<int>(text.size()), nullptr, 0);
  std::wstring result(size, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                      result.data(), size);
  return result;
}

// 将文件系统路径标准化为 UTF-8 编码字符串
auto path_utf8(const std::filesystem::path& path) -> std::string { return utf8(path.wstring()); }
}  // namespace utils::string

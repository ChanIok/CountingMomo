#include "utils/file.hpp"

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

#include "utils/string.hpp"

namespace utils::file {
auto read(const std::filesystem::path& path) -> std::expected<std::string, std::string> {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) return std::unexpected("无法读取：" + string::path_utf8(path));
  auto size = stream.tellg();
  if (size < 0) return std::unexpected("无法读取文件大小。");
  std::string data(static_cast<std::size_t>(size), '\0');
  stream.seekg(0);
  if (!data.empty() && !stream.read(data.data(), static_cast<std::streamsize>(data.size())))
    return std::unexpected("读取文件失败。");
  return data;
}
auto write(const std::filesystem::path& path, std::string_view text)
    -> std::expected<void, std::string> {
  std::ofstream stream(path, std::ios::binary);
  if (stream) stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  stream.flush();
  if (!stream) return std::unexpected("写入失败：" + string::path_utf8(path));
  return {};
}
auto executable_directory() -> std::filesystem::path {
  std::wstring buffer(32768, L'\0');
  auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  buffer.resize(length);
  return std::filesystem::path(buffer).parent_path();
}
auto timestamp() -> std::string {
  SYSTEMTIME time{};
  GetLocalTime(&time);
  return std::format("{:04}{:02}{:02}_{:02}{:02}{:02}_{:03}", time.wYear, time.wMonth, time.wDay,
                     time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
}
}  // namespace utils::file

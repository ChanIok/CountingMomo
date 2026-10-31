#include "utils/file.hpp"

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

#include "core/message/message.hpp"
#include "utils/string.hpp"

namespace utils::file {

// 二进制全量读取本地文件内容为 std::string
auto read(const std::filesystem::path& path) -> std::expected<std::string, core::Error> {
  // 以二进制并在末尾定位模式打开以快速获取长度
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    return std::unexpected(core::error("error.file.open_failed", string::path_utf8(path)));
  }
  auto size = stream.tellg();
  if (size < 0) {
    return std::unexpected(core::error("error.file.size_failed"));
  }
  std::string data(static_cast<std::size_t>(size), '\0');
  stream.seekg(0);
  // 读取全量文件数据
  if (!data.empty() && !stream.read(data.data(), static_cast<std::streamsize>(data.size()))) {
    return std::unexpected(core::error("error.file.read_failed"));
  }
  return data;
}

// 将字符串内容以二进制模式完整写入本地目标文件
auto write(const std::filesystem::path& path, std::string_view text)
    -> std::expected<void, core::Error> {
  std::ofstream stream(path, std::ios::binary);
  // 执行数据流写入并强制刷盘
  if (stream) {
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  }
  stream.flush();
  if (!stream) {
    return std::unexpected(core::error("error.file.write_failed", string::path_utf8(path)));
  }
  return {};
}

auto executable_path() -> std::expected<std::filesystem::path, core::Error> {
  std::wstring buffer(32768, L'\0');
  auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  // 路径不可用时明确失败，不能让资源或数据路径退化为相对工作目录。
  if (length == 0 || length >= buffer.size()) {
    auto error = length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
    return std::unexpected(core::with_detail(core::error("error.file.executable_directory"),
                                             std::format("GetModuleFileNameW: {}", error)));
  }
  buffer.resize(length);
  return std::filesystem::path(buffer);
}

auto executable_directory() -> std::expected<std::filesystem::path, core::Error> {
  auto path = executable_path();
  if (!path) {
    return std::unexpected(path.error());
  }
  return path->parent_path();
}

auto data_directory() -> std::expected<std::filesystem::path, core::Error> {
  auto directory = executable_directory();
  if (!directory) {
    return std::unexpected(directory.error());
  }
  auto root = *directory / "data";
  std::error_code error;
  std::filesystem::create_directories(root, error);
  if (error) {
    return std::unexpected(core::with_detail(
        core::error("error.app.create_data_directory", string::path_utf8(root)), error.message()));
  }
  return root;
}

// 生成当前本地时间的毫秒级格式化时间戳字符串（YYYYMMDD_HHMMSS_mmm）
auto timestamp() -> std::string {
  SYSTEMTIME time{};
  GetLocalTime(&time);
  return std::format("{:04}{:02}{:02}_{:02}{:02}{:02}_{:03}", time.wYear, time.wMonth, time.wDay,
                     time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
}

}  // namespace utils::file

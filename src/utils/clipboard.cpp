#include "utils/clipboard.hpp"

#include "vendor/std.hpp"

#include "core/message/message.hpp"
#include "utils/string.hpp"

namespace utils::clipboard {
// 将 UTF-8 字符串转换为 UTF-16 并安全写入 Windows 系统剪贴板
auto write_text(HWND owner, std::string_view utf8) -> std::expected<void, core::Error> {
  auto text = string::wide(utf8);
  auto bytes = (text.size() + 1) * sizeof(wchar_t);
  // 分配可移动全局内存块
  auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (!memory) {
    return std::unexpected(core::error("error.clipboard.allocate_failed"));
  }
  // 锁定内存并将宽字符内容拷入
  auto destination = GlobalLock(memory);
  if (!destination) {
    GlobalFree(memory);
    return std::unexpected(core::error("error.clipboard.lock_failed"));
  }
  std::memcpy(destination, text.c_str(), bytes);
  GlobalUnlock(memory);
  // 打开系统剪贴板通道
  if (!OpenClipboard(owner)) {
    GlobalFree(memory);
    return std::unexpected(core::error("error.clipboard.busy"));
  }
  // 清空旧数据并将全局内存句柄交接给系统（CF_UNICODETEXT）
  bool success = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
  auto error = GetLastError();
  CloseClipboard();
  if (!success) {
    GlobalFree(memory);
    return std::unexpected(core::error("error.clipboard.write_failed", error));
  }
  return {};
}
}  // namespace utils::clipboard

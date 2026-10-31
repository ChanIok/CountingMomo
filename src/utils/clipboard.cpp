#include "utils/clipboard.hpp"

#include "vendor/std.hpp"

#include "core/message/message.hpp"

#include "utils/string.hpp"

namespace utils::clipboard {
auto write_text(HWND owner, std::string_view utf8) -> std::expected<void, core::Error> {
  auto text = string::wide(utf8);
  auto bytes = (text.size() + 1) * sizeof(wchar_t);
  auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (!memory) {
    return std::unexpected(core::error("error.clipboard.allocate_failed"));
  }
  auto destination = GlobalLock(memory);
  if (!destination) {
    GlobalFree(memory);
    return std::unexpected(core::error("error.clipboard.lock_failed"));
  }
  std::memcpy(destination, text.c_str(), bytes);
  GlobalUnlock(memory);
  if (!OpenClipboard(owner)) {
    GlobalFree(memory);
    return std::unexpected(core::error("error.clipboard.busy"));
  }
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

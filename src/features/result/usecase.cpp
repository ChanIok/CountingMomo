#include "features/result/usecase.hpp"

#include "vendor/std.hpp"

#include "vendor/windows.hpp"
#include "vendor/windows/commdlg.hpp"

#include "core/i18n/i18n.hpp"
#include "core/message/message.hpp"
#include "core/state/app_state.hpp"
#include "features/result/result.hpp"
#include "features/result/state.hpp"
#include "features/scan/usecase.hpp"
#include "ui/main_window/state.hpp"
#include "ui/main_window/window.hpp"
#include "ui/status_window/window.hpp"
#include "utils/clipboard.hpp"

namespace features::result {

auto copy(core::AppState& state) -> void {
  // 运行中或无成功数据时不允许复制
  if (scan::running(state) || !state.result->last_success) {
    return;
  }
  // 将 27 个分类的 0/1 数组序列化为紧凑 JSON
  auto json = features::result::serialize(state.result->last_success->categories);
  auto copied = json ? utils::clipboard::write_text(state.main_window->hwnd, *json)
                     : std::expected<void, core::Error>(std::unexpected(json.error()));
  state.main_window->copied = copied.has_value();
  if (copied) {
    // 主按钮和完成通知直接反馈“已复制”，不在页脚重复提示同一结果。
    state.main_window->notice = {};
    state.main_window->notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    ui::main_window::refresh(state);
  } else {
    ui::main_window::notify(state, copied.error());
    ui::main_window::show(state);
  }
  ui::status_window::refresh(state);
}

auto save(core::AppState& state) -> void {
  if (scan::running(state) || !state.result->last_success) {
    return;
  }
  wchar_t path[32768] = L"result.json";
  OPENFILENAMEW dialog{sizeof(OPENFILENAMEW)};
  dialog.hwndOwner = state.main_window->hwnd;
  // 组装带多语言的扩展名过滤器描述
  auto filter = core::i18n::wide(state, "dialog.json_filter");
  filter.push_back(L'\0');
  filter.append(L"*.json");
  filter.append(2, L'\0');
  dialog.lpstrFilter = filter.c_str();
  dialog.lpstrFile = path;
  dialog.nMaxFile = static_cast<DWORD>(std::size(path));
  dialog.lpstrDefExt = L"json";
  dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  // 调起系统标准文件保存对话框
  if (!GetSaveFileNameW(&dialog)) {
    if (auto error = CommDlgExtendedError(); error != 0) {
      ui::main_window::notify(state, core::error("error.app.save_dialog", error));
    }
    return;
  }
  // 将结果数组写入目标文件
  auto saved = features::result::write(path, state.result->last_success->categories);
  if (!saved) {
    // 保存成功由文件本身与对话框关闭体现，页脚只保留失败原因。
    auto error = core::error("error.result.save_failed");
    error.related.push_back(saved.error());
    ui::main_window::notify(state, std::move(error));
  }
}
}  // namespace features::result

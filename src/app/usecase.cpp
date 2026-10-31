#include "app/usecase.hpp"

#include "vendor/std.hpp"

#include "vendor/rfl.hpp"
#include "vendor/windows.hpp"
#include "vendor/windows/commdlg.hpp"
#include "vendor/windows/shellapi.hpp"
#include "vendor/windows/shlobj.hpp"

#include "core/message/message.hpp"

#include "app/scan_job.hpp"
#include "app/state.hpp"
#include "core/i18n/i18n.hpp"
#include "core/state/app_state.hpp"
#include "features/result/result.hpp"
#include "features/scan/state.hpp"
#include "features/window/window.hpp"
#include "ui/main_window/state.hpp"
#include "ui/main_window/window.hpp"
#include "ui/status_window/state.hpp"
#include "ui/status_window/window.hpp"
#include "utils/clipboard.hpp"
#include "utils/file.hpp"
#include "utils/string.hpp"

namespace app {
namespace {
auto notice(core::AppState& state, core::Error message) -> void {
  state.app->copied = false;
  state.app->notice = std::move(message);
  state.app->notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(6);
  ui::main_window::refresh(state);
}
}  // namespace

auto initialize(core::AppState& state) -> std::expected<void, core::Error> {
  PWSTR folder{};
  auto hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &folder);
  if (FAILED(hr)) {
    return std::unexpected(core::error("error.app.data_directory"));
  }
  std::filesystem::path root(folder);
  CoTaskMemFree(folder);
  root /= "CountingMomo";
  std::error_code error;
  std::filesystem::create_directories(root, error);
  if (error) {
    return std::unexpected(
        core::with_detail(core::error("error.app.create_data_directory"), error.message()));
  }
  state.app->settings_path = root / "settings.json";
  bool exists = std::filesystem::exists(state.app->settings_path, error);
  if (error) {
    return std::unexpected(
        core::with_detail(core::error("error.settings.check_failed"), error.message()));
  }
  if (exists) {
    auto json = utils::file::read(state.app->settings_path);
    if (!json) {
      return std::unexpected(json.error());
    }
    auto settings = rfl::json::read<Settings>(*json);
    if (settings &&
        (settings->theme == "system" || settings->theme == "light" || settings->theme == "dark") &&
        (!settings->language || *settings->language == "zh-CN" || *settings->language == "en-US")) {
      state.app->settings = *settings;
    } else {
      state.app->notice = core::error("error.settings.invalid");
      state.app->notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    }
  }
  if (state.app->settings.language) {
    core::i18n::set_language(state, *state.app->settings.language == "zh-CN"
                                        ? core::i18n::Language::zh_cn
                                        : core::i18n::Language::en_us);
  }
  auto game = features::window::find(nullptr, L"X6Game-Win64-Shipping.exe");
  state.app->game = game ? *game : nullptr;
  return {};
}

auto running(const core::AppState& state) -> bool {
  return state.app->phase == Phase::running || state.app->phase == Phase::stopping;
}

auto start_scan(core::AppState& state) -> void {
  if (running(state)) {
    return;
  }
  auto& app = *state.app;
  auto game = features::window::find(nullptr, L"X6Game-Win64-Shipping.exe");
  if (!game) {
    app.phase = Phase::failed;
    app.message = game.error();
    ui::main_window::refresh(state);
    return;
  }
  app.game = *game;
  if (!RegisterHotKey(state.main_window->hwnd, 1, MOD_NOREPEAT, VK_F8)) {
    notice(state, core::error("error.app.register_hotkey"));
    return;
  }
  features::scan::Request request;
  auto root = utils::file::executable_directory();
  request.hwnd = app.game;
  request.layout = root / "resources/layout.json";
  request.categories = root / "resources/categories/categories.json";
  request.output = app.settings_path.parent_path() / "runs" / utils::file::timestamp();
  request.restore_window = app.settings.restore_window;
  state.scan->wake = [hwnd = state.main_window->hwnd] {
    PostMessageW(hwnd, ui::main_window::scan_update, 0, 0);
  };
  app.phase = Phase::running;
  app.progress = {};
  app.message = {};
  app.notice = {};
  app.copied = false;
  app.started = std::chrono::steady_clock::now();
  app.diagnostic = request.output / "diagnostics";
  state.main_window->page = ui::main_window::Page::main;
  state.main_window->scroll_y = 0;
  state.main_window->wheel_delta = 0;
  if (state.status_window->graphics_failed) {
    state.status_window->buttons = {};
    state.status_window->indicator = {};
    state.status_window->composition = {};
    state.status_window->redraw = true;
    state.status_window->repaint_buttons = true;
  }
  state.status_window->graphics_failed = false;
  if (auto started = scan_job::start(state, std::move(request)); !started) {
    UnregisterHotKey(state.main_window->hwnd, 1);
    app.phase = Phase::failed;
    app.message = started.error();
  } else {
    ShowWindow(state.main_window->hwnd, SW_MINIMIZE);
  }
  ui::main_window::refresh(state);
}

auto stop_scan(core::AppState& state) -> void {
  if (!running(state)) {
    return;
  }
  features::window::request_stop(state);
  state.app->phase = Phase::stopping;
  ui::main_window::refresh(state);
  ui::status_window::refresh(state);
}

auto refresh(core::AppState& state) -> void {
  auto result = scan_job::consume(state);
  auto& app = *state.app;
  if (result) {
    UnregisterHotKey(state.main_window->hwnd, 1);
    app.diagnostic = result->diagnostic;
    app.message = result->message;
    if (result->status == "completed") {
      app.phase = Phase::completed;
      app.success = std::move(*result);
      if (!app.success->warning.message.key.empty()) {
        app.notice = app.success->warning;
        app.notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
      }
    } else {
      app.phase = result->status == "stopped" ? Phase::stopped : Phase::failed;
    }
    if (app.closing) {
      DestroyWindow(state.main_window->hwnd);
      return;
    }
    ui::status_window::refresh(state, true);
  } else if (running(state)) {
    ui::status_window::refresh(state);
  }
  if ((!app.notice.message.key.empty() || app.copied) &&
      std::chrono::steady_clock::now() >= app.notice_until) {
    app.notice = {};
    app.copied = false;
  }
  ui::main_window::refresh(state);
}

auto copy_result(core::AppState& state) -> void {
  if (running(state) || !state.app->success) {
    return;
  }
  auto json = features::result::serialize(state.app->success->categories);
  auto copied = json ? utils::clipboard::write_text(state.main_window->hwnd, *json)
                     : std::expected<void, core::Error>(std::unexpected(json.error()));
  state.app->copied = copied.has_value();
  if (copied) {
    // 主按钮和完成通知直接反馈“已复制”，不在页脚重复提示同一结果。
    state.app->notice = {};
    state.app->notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    ui::main_window::refresh(state);
  } else {
    notice(state, copied.error());
    ui::main_window::show(state);
  }
  ui::status_window::refresh(state);
}

auto save_result(core::AppState& state) -> void {
  if (running(state) || !state.app->success) {
    return;
  }
  wchar_t path[32768] = L"result.json";
  OPENFILENAMEW dialog{sizeof(OPENFILENAMEW)};
  dialog.hwndOwner = state.main_window->hwnd;
  auto filter = core::i18n::wide(state, "dialog.json_filter");
  filter.push_back(L'\0');
  filter.append(L"*.json");
  filter.append(2, L'\0');
  dialog.lpstrFilter = filter.c_str();
  dialog.lpstrFile = path;
  dialog.nMaxFile = static_cast<DWORD>(std::size(path));
  dialog.lpstrDefExt = L"json";
  dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetSaveFileNameW(&dialog)) {
    if (auto error = CommDlgExtendedError(); error != 0) {
      notice(state, core::error("error.app.save_dialog", error));
    }
    return;
  }
  auto saved = features::result::write(path, state.app->success->categories);
  if (!saved) {
    // 保存成功由文件本身与对话框关闭体现，页脚只保留失败原因。
    notice(state, saved.error());
  }
}

auto open_diagnostic(core::AppState& state) -> void {
  if (running(state)) {
    return;
  }
  std::error_code error;
  if (state.app->diagnostic.empty() ||
      !std::filesystem::is_directory(state.app->diagnostic, error)) {
    notice(state, core::error("error.app.no_diagnostics"));
    return;
  }
  auto result = ShellExecuteW(state.main_window->hwnd, L"open", state.app->diagnostic.c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(result) <= 32) {
    notice(state, core::error("error.app.open_diagnostics"));
  }
}

auto save_settings(core::AppState& state) -> void {
  try {
    auto temporary = state.app->settings_path;
    temporary += L".tmp";
    auto written = utils::file::write(temporary, rfl::json::write(state.app->settings));
    if (!written) {
      notice(state, written.error());
      return;
    }
    if (!MoveFileExW(temporary.c_str(), state.app->settings_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      notice(state, core::error("error.settings.publish_failed"));
    }
  } catch (const std::exception& error) {
    notice(state, core::with_detail(core::error("error.settings.save_failed"), error.what()));
  }
}

auto close(core::AppState& state) -> void {
  if (running(state)) {
    state.app->closing = true;
    stop_scan(state);
    SetWindowTextW(state.main_window->hwnd, core::i18n::wide(state, "app.closing").c_str());
  } else {
    DestroyWindow(state.main_window->hwnd);
  }
}
}  // namespace app

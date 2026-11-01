#include "features/settings/usecase.hpp"

#include "vendor/std.hpp"

#include "vendor/rfl.hpp"
#include "vendor/windows.hpp"
#include "vendor/windows/shellapi.hpp"

#include "core/i18n/i18n.hpp"
#include "core/message/message.hpp"
#include "core/state/app_state.hpp"
#include "features/scan/control.hpp"
#include "features/scan/usecase.hpp"
#include "features/settings/state.hpp"
#include "ui/main_window/state.hpp"
#include "ui/main_window/window.hpp"
#include "utils/file.hpp"
#include "utils/string.hpp"

namespace features::settings {
namespace {
// 项目仓库地址；设置页入口固定打开此页，供查看源码与反馈问题。
constexpr wchar_t repository_url[] = L"https://github.com/ChanIok/CountingMomo";
}  // namespace

auto initialize(core::AppState& state) -> std::expected<void, core::Error> {
  auto root = utils::file::data_directory();
  if (!root) {
    return std::unexpected(root.error());
  }
  state.settings->path = *root / "settings.json";
  std::error_code error;
  bool exists = std::filesystem::exists(state.settings->path, error);
  if (error) {
    return std::unexpected(
        core::with_detail(core::error("error.settings.check_failed"), error.message()));
  }
  // 若配置文件存在则读取反序列化，校验合法性后载入
  if (exists) {
    auto json = utils::file::read(state.settings->path);
    if (!json) {
      return std::unexpected(json.error());
    }
    auto settings = rfl::json::read<Settings>(*json);
    // 校验主题与语言选项的合法枚举值
    if (settings &&
        (settings->theme == "system" || settings->theme == "light" || settings->theme == "dark") &&
        (!settings->language || *settings->language == "zh-CN" || *settings->language == "en-US") &&
        settings->stop_key.get() >= 1 && settings->stop_key.get() <= 11) {
      state.settings->values = *settings;
    } else {
      // 配置损坏或字段非法时发出告警，保留 12 秒
      state.main_window->notice = core::error("error.settings.invalid");
      state.main_window->notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    }
  }
  // 应用用户预设的多语言配置
  if (state.settings->values.language) {
    core::i18n::set_language(state, *state.settings->values.language == "zh-CN"
                                        ? core::i18n::Language::zh_cn
                                        : core::i18n::Language::en_us);
  }
  return {};
}

auto save(core::AppState& state) -> void {
  try {
    auto temporary = state.settings->path;
    temporary += L".tmp";
    // 写入临时文件，防止写入中断损坏原配置
    auto written = utils::file::write(temporary, rfl::json::write(state.settings->values));
    if (!written) {
      auto error =
          core::error("error.settings.save_failed", utils::string::path_utf8(state.settings->path));
      error.related.push_back(written.error());
      ui::main_window::notify(state, std::move(error));
      return;
    }
    // 原子覆盖替换并在磁盘上写穿生效
    if (!MoveFileExW(temporary.c_str(), state.settings->path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      auto error = GetLastError();
      ui::main_window::notify(
          state, core::with_detail(core::error("error.settings.publish_failed",
                                               utils::string::path_utf8(state.settings->path)),
                                   std::format("MoveFileExW: {}", error)));
    }
  } catch (const std::exception& error) {
    ui::main_window::notify(
        state, core::with_detail(core::error("error.settings.save_failed",
                                             utils::string::path_utf8(state.settings->path)),
                                 error.what()));
  }
}

auto cycle_theme(core::AppState& state) -> std::expected<void, core::Error> {
  auto& mode = state.settings->values.theme;
  mode = mode == "system" ? "light" : mode == "light" ? "dark" : "system";
  save(state);
  return ui::main_window::update_theme(state);
}

auto toggle_language(core::AppState& state) -> void {
  auto language = core::i18n::current(state) == core::i18n::Language::zh_cn
                      ? core::i18n::Language::en_us
                      : core::i18n::Language::zh_cn;
  core::i18n::set_language(state, language);
  state.settings->values.language = std::string(core::i18n::locale(language));
  save(state);
  ui::main_window::update_language(state);
}

auto set_stop_key(core::AppState& state, int key) -> void {
  if (scan::running(state) || key < 1 || key > 11) {
    return;
  }
  state.settings->values.stop_key = key;
  save(state);
  scan::check_hotkey(state);
  ui::main_window::refresh(state);
}

auto open_repository(core::AppState& state) -> void {
  // 交由系统默认浏览器打开仓库页；失败只提示，不影响设置本身。
  auto result = ShellExecuteW(state.main_window->hwnd, L"open", repository_url, nullptr, nullptr,
                              SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(result) <= 32) {
    ui::main_window::notify(state, core::error("error.app.open_repository"));
  }
}
}  // namespace features::settings

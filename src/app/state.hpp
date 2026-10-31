#pragma once
#include "vendor/std.hpp"

#include "vendor/windows.hpp"

#include "core/message/types.hpp"

#include "features/scan/state.hpp"

namespace app {
enum class Phase { ready, running, stopping, completed, stopped, failed };

struct Settings {
  std::string theme{"system"};
  std::optional<std::string> language;  // 未手动选择时按系统显示语言决定默认值。
  bool restore_window{true};
};

struct State {
  // 除 settings 启动前复制到请求外，以下均只由 UI 线程读写。
  Phase phase{Phase::ready};
  Settings settings;
  features::scan::Progress progress;
  std::optional<features::scan::Outcome> success;
  std::filesystem::path diagnostic;
  std::filesystem::path settings_path;
  core::Error message;
  core::Error notice;
  HWND game{};
  bool closing{};
  bool copied{};
  std::chrono::steady_clock::time_point started{};
  std::chrono::steady_clock::time_point notice_until{};
};
}  // namespace app

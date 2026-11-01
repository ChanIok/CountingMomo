#pragma once

#include "vendor/std.hpp"

#include "vendor/windows.hpp"
#include "vendor/windows/shobjidl_core.hpp"

#include "features/scan/task_types.hpp"
#include "ui/button.hpp"
#include "ui/composition.hpp"
#include "ui/main_window/layout.hpp"

namespace ui::main_window {

enum class Page { main, settings, details };

// 可见内容的快照；同一快照供布局、绘制和按钮模型使用，不从 HWND 回读数据。
struct View {
  Page page{};
  features::scan::Phase phase{};
  std::wstring category;
  std::wstring message;
  std::wstring notice;
  std::wstring warning;
  std::wstring theme_label;
  std::wstring repository_label;
  std::wstring language_label;
  std::wstring stop_key_label;
  std::wstring stop_key_button_label;
  std::wstring hotkey_warning;
  bool hotkey_available{true};
  button::Action focused{};
  int summary_count{};
  int category_count{};
  int total{};
  int owned{};
  long long elapsed{};  // 扫描中按整秒更新。
  double completed_seconds{};
  bool has_success{};
  bool diagnostic{};
  bool copied{};
  bool game_found{};

  auto operator==(const View&) const -> bool = default;
};

struct State {
  core::Error notice;
  std::chrono::steady_clock::time_point notice_until{};
  bool copied{};
  bool closing{};
  HWND hwnd{};
  UINT taskbar_message{};
  Microsoft::WRL::ComPtr<ITaskbarList3> taskbar;
  TBPFLAG taskbar_progress{TBPF_NOPROGRESS};
  UINT dpi{96};
  UINT width{};  // 客户区原生像素。
  UINT height{};
  float scroll_y{};
  int wheel_delta{};
  bool material{};
  Page page{};
  bool graphics_failed{};
  bool redraw{true};
  bool repaint_buttons{true};
  View view;
  layout::Layout layout;
  composition::Host composition;
  button::State buttons;
  button::Action focused{};
  bool recording_stop_key{};
  int stop_key_candidate{};  // 0 表示尚未按下有效功能键；松开前不写入设置。
};
}  // namespace ui::main_window

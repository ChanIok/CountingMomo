#pragma once
#include "vendor/std.hpp"

#include "vendor/windows.hpp"
#include "vendor/windows/shobjidl_core.hpp"

#include "app/state.hpp"
#include "ui/button.hpp"
#include "ui/composition.hpp"
#include "ui/main_window/layout.hpp"

namespace ui::main_window {

enum class Page { main, settings, details };

// 可见内容的快照；同一快照供布局、绘制和按钮模型使用，不从 HWND 回读数据。
struct View {
  Page page{};
  app::Phase phase{};
  std::wstring category;
  std::wstring message;
  std::wstring notice;
  std::wstring warning;
  std::wstring theme_label;
  std::wstring restore_label;
  std::wstring language_label;
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
};
}  // namespace ui::main_window

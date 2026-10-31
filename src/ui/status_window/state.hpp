#pragma once
#include "vendor/std.hpp"

#include "vendor/windows.hpp"

#include "ui/button.hpp"
#include "ui/composition.hpp"

namespace ui::status_window {
struct State {
  HWND hwnd{};
  UINT dpi{96};
  bool material{};
  bool visible{};
  bool graphics_failed{};
  bool redraw{true};
  bool repaint_buttons{true};
  bool active{};
  bool success{};
  bool copied{};
  bool failed{};
  std::wstring title;
  std::wstring detail;
  composition::Host composition;
  composition::Layer indicator;
  button::State buttons;
  std::chrono::steady_clock::time_point expires{};
};
}  // namespace ui::status_window

#pragma once
#include "vendor/std.hpp"

namespace core::i18n {
struct State;
}

namespace app {
struct State;
}

namespace features::scan {
struct State;
}

namespace features::window {
struct State;
}

namespace ui::shared_render_resources {
struct State;
}

namespace ui::theme {
struct State;
}

namespace ui::main_window {
struct State;
}

namespace ui::status_window {
struct State;
}

namespace core {
// 只在这里聚合所有权；访问子状态的实现文件显式包含对应 state.hpp。
struct AppState {
  AppState();
  ~AppState();
  std::unique_ptr<app::State> app;
  std::unique_ptr<i18n::State> i18n;
  std::unique_ptr<features::scan::State> scan;
  std::unique_ptr<features::window::State> window;
  std::unique_ptr<ui::theme::State> theme;
  std::unique_ptr<ui::shared_render_resources::State> shared_render_resources;
  std::unique_ptr<ui::main_window::State> main_window;
  std::unique_ptr<ui::status_window::State> status_window;
};
}  // namespace core

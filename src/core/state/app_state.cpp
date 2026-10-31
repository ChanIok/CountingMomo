#include "core/state/app_state.hpp"

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

#include "app/state.hpp"
#include "core/i18n/state.hpp"
#include "features/scan/state.hpp"
#include "features/window/state.hpp"
#include "ui/main_window/state.hpp"
#include "ui/shared_render_resources/state.hpp"
#include "ui/status_window/state.hpp"
#include "ui/theme.hpp"
#include "ui/theme/state.hpp"

namespace core {
AppState::AppState()
    : app(std::make_unique<::app::State>()),
      i18n(std::make_unique<core::i18n::State>()),
      scan(std::make_unique<features::scan::State>()),
      window(std::make_unique<features::window::State>()),
      theme(std::make_unique<ui::theme::State>()),
      shared_render_resources(std::make_unique<ui::shared_render_resources::State>()),
      main_window(std::make_unique<ui::main_window::State>()),
      status_window(std::make_unique<ui::status_window::State>()) {}

AppState::~AppState() {
  ui::theme::shutdown(*this);
  scan->stop.store(true);
  if (scan->worker.joinable()) {
    scan->worker.join();
  }
  if (status_window->hwnd) {
    DestroyWindow(status_window->hwnd);
  }
  if (main_window->hwnd) {
    DestroyWindow(main_window->hwnd);
  }
}
}  // namespace core

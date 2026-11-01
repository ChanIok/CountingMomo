#include "core/initializer/initializer.hpp"

#include "vendor/std.hpp"

#include "core/i18n/i18n.hpp"
#include "core/startup/startup.hpp"
#include "core/state/app_state.hpp"
#include "features/scan/control.hpp"
#include "features/settings/usecase.hpp"
#include "features/window/state.hpp"
#include "features/window/window.hpp"
#include "ui/composition.hpp"
#include "ui/main_window/state.hpp"
#include "ui/main_window/window.hpp"
#include "ui/status_window/window.hpp"
#include "ui/theme.hpp"

namespace core::initializer {

auto initialize_application(AppState& state, const wchar_t* arguments, int show_command)
    -> std::expected<startup::Action, Error> {
  i18n::set_language(state, i18n::system_language());
  auto action = startup::prepare(state, arguments, show_command);
  if (!action) {
    return std::unexpected(action.error());
  }
  if (*action == startup::Action::activate_existing) {
    if (auto activated = ui::main_window::activate_existing(); !activated) {
      return std::unexpected(activated.error());
    }
    return startup::Action::exit;
  }
  if (*action == startup::Action::exit) {
    return startup::Action::exit;
  }
  if (auto initialized = features::settings::initialize(state); !initialized) {
    return std::unexpected(initialized.error());
  }

  auto game = features::window::find(nullptr, features::window::game_executable);
  state.window->game = game ? *game : nullptr;

  if (auto initialized = ui::theme::initialize(state); !initialized) {
    return std::unexpected(initialized.error());
  }
  if (auto initialized = ui::composition::initialize(state); !initialized) {
    return std::unexpected(initialized.error());
  }
  if (auto initialized = ui::main_window::create(state); !initialized) {
    return std::unexpected(initialized.error());
  }
  if (auto initialized =
          ui::theme::watch(state, state.main_window->hwnd, ui::main_window::theme_update);
      !initialized) {
    return std::unexpected(initialized.error());
  }
  if (auto initialized = ui::status_window::create(state); !initialized) {
    return std::unexpected(initialized.error());
  }
  features::scan::check_hotkey(state);
  ui::main_window::refresh(state);
  return startup::Action::run;
}

}  // namespace core::initializer

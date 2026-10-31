#include "vendor/std.hpp"

#include "vendor/windows.hpp"
#include "vendor/windows/roapi.hpp"

#include "app/usecase.hpp"
#include "core/i18n/i18n.hpp"
#include "core/state/app_state.hpp"
#include "ui/composition.hpp"
#include "ui/main_window/state.hpp"
#include "ui/main_window/window.hpp"
#include "ui/status_window/window.hpp"
#include "ui/theme.hpp"
#include "utils/string.hpp"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int show_command) {
  auto apartment = RoInitialize(RO_INIT_SINGLETHREADED);
  if (FAILED(apartment)) {
    MessageBoxW(nullptr,
                utils::string::wide(core::i18n::text(core::i18n::system_language(),
                                                     core::error("error.app.com_initialize")))
                    .c_str(),
                L"CountingMomo", MB_OK | MB_ICONERROR);
    return 1;
  }
  int exit_code = 0;
  auto language = core::i18n::system_language();
  try {
    core::AppState state;
    core::i18n::set_language(state, core::i18n::system_language());
    auto initialized = app::initialize(state);
    language = core::i18n::current(state);
    if (initialized) {
      initialized = ui::theme::initialize(state);
    }
    if (initialized) {
      initialized = ui::composition::initialize(state);
    }
    if (initialized) {
      initialized = ui::main_window::create(state);
    }
    if (initialized) {
      initialized = ui::theme::watch(state, state.main_window->hwnd, ui::main_window::theme_update);
    }
    if (initialized) {
      initialized = ui::status_window::create(state);
    }
    language = core::i18n::current(state);
    if (!initialized) {
      MessageBoxW(state.main_window->hwnd, core::i18n::wide(state, initialized.error()).c_str(),
                  core::i18n::wide(state, "error.app.startup_title").c_str(), MB_OK | MB_ICONERROR);
      exit_code = 1;
    } else {
      ShowWindow(state.main_window->hwnd, show_command);
      UpdateWindow(state.main_window->hwnd);
      MSG message{};
      int received{};
      while ((received = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
      exit_code = received < 0 ? 1 : static_cast<int>(message.wParam);
    }
  } catch (const std::exception& error) {
    MessageBoxW(nullptr,
                utils::string::wide(
                    core::i18n::text(language, core::with_detail(core::error("error.app.exception"),
                                                                 error.what())))
                    .c_str(),
                L"CountingMomo", MB_OK | MB_ICONERROR);
    exit_code = 1;
  }
  RoUninitialize();
  return exit_code;
}

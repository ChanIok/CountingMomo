#include "vendor/std.hpp"

#include "vendor/windows.hpp"
#include "vendor/windows/roapi.hpp"

#include "core/i18n/i18n.hpp"
#include "core/initializer/initializer.hpp"
#include "core/shutdown/shutdown.hpp"
#include "core/state/app_state.hpp"
#include "ui/main_window/state.hpp"
#include "utils/string.hpp"

namespace {

auto run_application(core::AppState& state, const wchar_t* arguments, int show_command) -> int {
  auto initialized = core::initializer::initialize_application(state, arguments, show_command);
  if (!initialized) {
    MessageBoxW(state.main_window->hwnd, core::i18n::wide(state, initialized.error()).c_str(),
                core::i18n::wide(state, "error.app.startup_title").c_str(), MB_OK | MB_ICONERROR);
    return 1;
  }
  if (*initialized == core::startup::Action::exit) {
    return 0;
  }

  ShowWindow(state.main_window->hwnd, show_command);
  UpdateWindow(state.main_window->hwnd);
  MSG message{};
  int received{};
  while ((received = GetMessageW(&message, nullptr, 0, 0)) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return received < 0 ? 1 : static_cast<int>(message.wParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR arguments, int show_command) {
  // UI 资源的显式关闭和自然析构都必须发生在当前 COM 套间撤销之前。
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
    try {
      exit_code = run_application(state, arguments, show_command);
    } catch (...) {
      language = core::i18n::current(state);
      // 初始化中途或消息循环抛出异常时，也先收尾，不能依赖 AppState 析构停止任务。
      core::shutdown::shutdown_application(state);
      throw;
    }
    language = core::i18n::current(state);
    core::shutdown::shutdown_application(state);
  } catch (const std::exception& error) {
    MessageBoxW(nullptr,
                utils::string::wide(
                    core::i18n::text(language, core::with_detail(core::error("error.app.exception"),
                                                                 error.what())))
                    .c_str(),
                L"CountingMomo", MB_OK | MB_ICONERROR);
    exit_code = 1;
  } catch (...) {
    MessageBoxW(
        nullptr,
        utils::string::wide(core::i18n::text(language, core::error("error.app.exception"))).c_str(),
        L"CountingMomo", MB_OK | MB_ICONERROR);
    exit_code = 1;
  }
  RoUninitialize();
  return exit_code;
}

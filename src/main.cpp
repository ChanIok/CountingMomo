#include "vendor/std.hpp"

#include "vendor/windows.hpp"
#include "vendor/windows/roapi.hpp"

#include "cli.hpp"
#include "features/scan/usecase.hpp"
#include "features/window/window.hpp"

namespace {
auto WINAPI console_event(DWORD event) -> BOOL {
  if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
    features::window::request_stop();
    return TRUE;
  }
  return FALSE;
}
}  // namespace
int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
  SetConsoleCtrlHandler(console_event, TRUE);
  auto apartment = RoInitialize(RO_INIT_MULTITHREADED);
  if (FAILED(apartment)) {
    cli::log("COM初始化失败。");
    return 1;
  }
  int exit_code = 1;
  try {
    auto options = cli::parse(argc, argv);
    if (!options)
      cli::log(options.error());
    else if (options->command == "help") {
      cli::print_help();
      exit_code = 0;
    } else {
      auto result = features::scan::run(*options);
      if (result)
        exit_code = *result;
      else
        cli::log("无法完成：" + result.error());
    }
  } catch (const std::exception& error) {
    cli::log(std::string("无法完成：") + error.what());
  }
  RoUninitialize();
  return exit_code;
}

#include "core/startup/startup.hpp"

#include "vendor/std.hpp"

#include "vendor/windows.hpp"
#include "vendor/windows/shellapi.hpp"

#include "core/message/message.hpp"
#include "core/startup/state.hpp"
#include "core/state/app_state.hpp"
#include "utils/file.hpp"

namespace core::startup {
namespace {

constexpr auto instance_mutex_name = L"Local\\CountingMomo.SingleInstance";

auto is_elevated() -> std::expected<bool, Error> {
  HANDLE token{};
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    auto error = GetLastError();
    return std::unexpected(core::with_detail(core::error("error.startup.check_privileges"),
                                             std::format("OpenProcessToken: {}", error)));
  }
  TOKEN_ELEVATION elevation{};
  DWORD size{};
  auto queried = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
  auto error = queried ? ERROR_SUCCESS : GetLastError();
  CloseHandle(token);
  if (!queried) {
    return std::unexpected(core::with_detail(core::error("error.startup.check_privileges"),
                                             std::format("GetTokenInformation: {}", error)));
  }
  return elevation.TokenIsElevated != 0;
}

}  // namespace

auto prepare(AppState& state, const wchar_t* arguments, int show_command)
    -> std::expected<Action, Error> {
  // 仅用命名对象的存在表示实例存活，不请求所有权或修改权限。
  auto mutex = CreateMutexExW(nullptr, instance_mutex_name, 0, SYNCHRONIZE);
  auto error = GetLastError();
  if (!mutex) {
    return std::unexpected(core::with_detail(core::error("error.startup.single_instance"),
                                             std::format("CreateMutexExW: {}", error)));
  }
  if (error == ERROR_ALREADY_EXISTS) {
    CloseHandle(mutex);
    return Action::activate_existing;
  }
  state.startup->instance_mutex = mutex;

  auto elevated = is_elevated();
  if (!elevated) {
    return std::unexpected(elevated.error());
  }
  if (*elevated) {
    return Action::run;
  }

  auto executable = utils::file::executable_path();
  if (!executable) {
    return std::unexpected(executable.error());
  }
  auto directory = executable->parent_path();
  // 管理员子进程会重新争用同一个命名对象，不能被启动进程自己的句柄挡住。
  release_instance(state);
  SHELLEXECUTEINFOW execute{sizeof(SHELLEXECUTEINFOW)};
  execute.fMask = SEE_MASK_NOASYNC;
  execute.lpVerb = L"runas";
  execute.lpFile = executable->c_str();
  execute.lpParameters = arguments;
  execute.lpDirectory = directory.c_str();
  execute.nShow = show_command;
  if (!ShellExecuteExW(&execute)) {
    error = GetLastError();
    if (error != ERROR_CANCELLED) {
      return std::unexpected(core::with_detail(core::error("error.startup.elevate"),
                                               std::format("ShellExecuteExW: {}", error)));
    }
  }
  // 请求已交给管理员子进程，或用户明确取消；当前进程都不进入业务初始化。
  return Action::exit;
}

auto release_instance(AppState& state) -> void {
  if (state.startup->instance_mutex) {
    CloseHandle(state.startup->instance_mutex);
    state.startup->instance_mutex = nullptr;
  }
}

}  // namespace core::startup

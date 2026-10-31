#include "features/window/window.hpp"

#include "vendor/std.hpp"

#include "vendor/windows/tlhelp32.hpp"

#include "core/message/message.hpp"

#include "core/state/app_state.hpp"
#include "features/scan/diagnostics.hpp"
#include "features/scan/state.hpp"
#include "features/window/state.hpp"
#include "utils/string.hpp"

namespace features::window {

namespace {

auto error(std::string_view operation) -> core::Error {
  auto code = GetLastError();
  return core::error("error.window.operation_failed", core::text_key(std::string(operation)), code);
}

struct Process {
  DWORD pid{};
  std::wstring exe;
};

// 进程名只做 ASCII 大小写折叠；游戏可执行文件名不含非 ASCII 字符。
auto ascii_lower(std::wstring_view text) -> std::wstring {
  std::wstring value(text);
  for (auto& c : value) {
    if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c - L'A' + L'a');
    }
  }
  return value;
}

// 单次进程快照按名称定位，避免逐窗口 OpenProcess。
auto processes() -> std::vector<Process> {
  std::vector<Process> result;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return result;
  }
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snapshot, &entry)) {
    do {
      result.push_back({entry.th32ProcessID, entry.szExeFile});
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return result;
}

struct Lookup {
  const std::vector<DWORD>* pids{};
  HWND hwnd{};
  LONG area{};
};

}  // namespace

auto request_stop(core::AppState& state) -> void { state.scan->stop.store(true); }

auto stop_requested(core::AppState& state) -> bool {
  if (GetAsyncKeyState(VK_F8) & 0x8000) {
    state.scan->stop.store(true);
  }
  return state.scan->stop.load();
}

auto restore(core::AppState& state) -> std::expected<void, core::Error> {
  auto& original = state.window->original;
  if (!original) {
    return {};
  }
  DWORD pid{};
  GetWindowThreadProcessId(original->hwnd, &pid);
  if (!IsWindow(original->hwnd) || pid != original->pid) {
    original.reset();
    return {};  // 原窗口已销毁，不能把旧样式写给复用句柄的新窗口。
  }
  SetLastError(0);
  if (!SetWindowLongPtrW(original->hwnd, GWL_STYLE, original->style) && GetLastError()) {
    return std::unexpected(error("window.operation.restore_style"));
  }
  SetLastError(0);
  if (!SetWindowLongPtrW(original->hwnd, GWL_EXSTYLE, original->ex_style) && GetLastError()) {
    return std::unexpected(error("window.operation.restore_ex_style"));
  }
  if (!SetWindowPos(original->hwnd, nullptr, 0, 0, 0, 0,
                    SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) ||
      !SetWindowPlacement(original->hwnd, &original->placement)) {
    return std::unexpected(error("window.operation.restore_position"));
  }
  original.reset();
  return {};
}

auto find(HWND explicit_handle, std::wstring_view exe) -> std::expected<HWND, core::Error> {
  if (explicit_handle) {
    if (!IsWindow(explicit_handle)) {
      return std::unexpected(core::error("error.window.invalid_handle"));
    }
    return explicit_handle;
  }
  auto wanted = ascii_lower(exe);
  if (wanted.empty()) {
    return std::unexpected(core::error("error.window.empty_process_name"));
  }
  std::vector<DWORD> pids;
  for (const auto& process : processes()) {
    if (ascii_lower(process.exe) == wanted) {
      pids.push_back(process.pid);
    }
  }
  if (pids.empty()) {
    return std::unexpected(core::error("error.window.process_not_found", utils::string::utf8(exe)));
  }
  Lookup lookup{&pids};
  // 游戏会创建额外的顶层辅助窗口，主窗口取可见、无 owner、面积最大者。
  EnumWindows(
      [](HWND hwnd, LPARAM data) -> BOOL {
        auto& holder = *reinterpret_cast<Lookup*>(data);
        DWORD pid{};
        GetWindowThreadProcessId(hwnd, &pid);
        if (std::ranges::find(*holder.pids, pid) == holder.pids->end()) {
          return TRUE;
        }
        if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) {
          return TRUE;
        }
        // 最小化时 GetWindowRect 落在屏幕外，面积不可比，改用还原矩形。
        RECT rect{};
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        if (IsIconic(hwnd)) {
          if (!GetWindowPlacement(hwnd, &placement)) {
            return TRUE;
          }
          rect = placement.rcNormalPosition;
        } else if (!GetWindowRect(hwnd, &rect)) {
          return TRUE;
        }
        auto area = (rect.right - rect.left) * (rect.bottom - rect.top);
        if (area > holder.area) {
          holder.area = area;
          holder.hwnd = hwnd;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&lookup));
  if (!lookup.hwnd) {
    return std::unexpected(core::error("error.window.no_main_window", utils::string::utf8(exe)));
  }
  return lookup.hwnd;
}

auto geometry(HWND hwnd) -> std::expected<Geometry, core::Error> {
  RECT client_rect{};
  POINT origin{};
  if (!IsWindow(hwnd) || IsIconic(hwnd)) {
    return std::unexpected(core::error("error.window.unavailable"));
  }
  if (!GetClientRect(hwnd, &client_rect) || !ClientToScreen(hwnd, &origin)) {
    return std::unexpected(error("window.operation.read_client"));
  }
  return Geometry{client_rect.right, client_rect.bottom, origin};
}

auto prepare(core::AppState& state, HWND hwnd, int width, int height, double timeout)
    -> std::expected<Target, core::Error> {
  if (stop_requested(state)) {
    return std::unexpected(core::error("error.scan.stopped"));
  }
  if (state.window->restore_enabled && !state.window->original) {
    SavedWindow saved{.hwnd = hwnd};
    GetWindowThreadProcessId(hwnd, &saved.pid);
    SetLastError(0);
    saved.style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (!saved.style && GetLastError()) {
      return std::unexpected(error("window.operation.save_style"));
    }
    SetLastError(0);
    saved.ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (!saved.ex_style && GetLastError()) {
      return std::unexpected(error("window.operation.save_ex_style"));
    }
    if (!saved.pid || !GetWindowPlacement(hwnd, &saved.placement)) {
      return std::unexpected(error("window.operation.save_position"));
    }
    state.window->original = saved;
  }
  using Clock = std::chrono::steady_clock;
  auto deadline = Clock::now() + std::chrono::duration<double>(timeout);
  if (IsIconic(hwnd) || IsZoomed(hwnd)) {
    ShowWindowAsync(hwnd, SW_RESTORE);
  }
  while (IsIconic(hwnd) || IsZoomed(hwnd)) {
    if (stop_requested(state)) {
      return std::unexpected(core::error("error.scan.stopped"));
    }
    if (Clock::now() >= deadline) {
      return std::unexpected(core::error("error.window.restore_timeout"));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  MONITORINFO monitor{sizeof(MONITORINFO)};
  if (!GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor)) {
    return std::unexpected(error("window.operation.read_monitor"));
  }
  auto x = monitor.rcMonitor.left + (monitor.rcMonitor.right - monitor.rcMonitor.left - width) / 2;
  auto y = monitor.rcMonitor.top;
  SetLastError(0);
  auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  if (!style && GetLastError()) {
    return std::unexpected(error("window.operation.read_style"));
  }
  SetLastError(0);
  if (!SetWindowLongPtrW(hwnd, GWL_STYLE,
                         (style & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW)) | WS_POPUP) &&
      GetLastError()) {
    return std::unexpected(error("window.operation.set_borderless"));
  }
  if (!SetWindowPos(hwnd, nullptr, x, y, width, height,
                    SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS)) {
    return std::unexpected(error("window.operation.resize"));
  }
  std::optional<Clock::time_point> since;
  while (Clock::now() < deadline) {
    if (stop_requested(state)) {
      return std::unexpected(core::error("error.scan.stopped"));
    }
    auto actual = geometry(hwnd);
    if (!actual) {
      return std::unexpected(actual.error());
    }
    auto now = Clock::now();
    if (actual->width == width && actual->height == height && actual->origin.x == x &&
        actual->origin.y == y) {
      if (!since) {
        since = now;
      }
      if (now - *since >= std::chrono::milliseconds(250)) {
        DWORD pid{};
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid) {
          return std::unexpected(core::error("error.window.process_exited"));
        }
        scan::diagnostics::log(core::message("log.window.prepared", width, height, x, y));
        return Target{hwnd, pid, width, height};
      }
    } else {
      since.reset();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  return std::unexpected(core::error("error.window.resize_timeout"));
}

auto check(core::AppState& state, const Target& target, bool foreground)
    -> std::expected<void, core::Error> {
  if (stop_requested(state)) {
    return std::unexpected(core::error("error.scan.cancelled"));
  }
  DWORD pid{};
  GetWindowThreadProcessId(target.hwnd, &pid);
  if (pid != target.pid) {
    return std::unexpected(core::error("error.window.process_changed"));
  }
  auto size = geometry(target.hwnd);
  if (!size) {
    return std::unexpected(size.error());
  }
  if (size->width != target.width || size->height != target.height) {
    return std::unexpected(core::error("error.window.size_changed"));
  }
  if (foreground && GetForegroundWindow() != target.hwnd) {
    return std::unexpected(core::error("error.window.focus_lost"));
  }
  return {};
}

auto activate(core::AppState& state, const Target& target) -> std::expected<void, core::Error> {
  using Clock = std::chrono::steady_clock;
  if (auto ok = check(state, target, false); !ok) {
    return ok;
  }
  scan::diagnostics::log(core::message("log.window.activating"));
  SetForegroundWindow(target.hwnd);
  auto deadline = Clock::now() + std::chrono::seconds(30);
  std::optional<Clock::time_point> since;
  while (Clock::now() < deadline) {
    if (auto ok = check(state, target, false); !ok) {
      return ok;
    }
    if (GetForegroundWindow() == target.hwnd) {
      if (!since) {
        since = Clock::now();
      }
      if (Clock::now() - *since >= std::chrono::milliseconds(200)) {
        return {};
      }
    } else {
      since.reset();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  return std::unexpected(core::error("error.window.focus_timeout"));
}

auto scroll_point(const Target& target, const scan::Box& bounds)
    -> std::expected<POINT, core::Error> {
  auto size = geometry(target.hwnd);
  if (!size) {
    return std::unexpected(size.error());
  }
  if (bounds[0] < 0 || bounds[1] < 0 || bounds[2] > target.width || bounds[3] > target.height ||
      bounds[0] >= bounds[2] || bounds[1] >= bounds[3]) {
    return std::unexpected(core::error("error.window.invalid_scroll_region"));
  }

  struct Visible {
    RECT region;
    std::vector<RECT> intersections;
  };

  Visible visible{{bounds[0] + size->origin.x, bounds[1] + size->origin.y,
                   bounds[2] + size->origin.x, bounds[3] + size->origin.y},
                  {}};
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR, HDC, LPRECT screen, LPARAM data) -> BOOL {
        auto& value = *reinterpret_cast<Visible*>(data);
        RECT overlap{};
        if (IntersectRect(&overlap, &value.region, screen)) {
          value.intersections.push_back(overlap);
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&visible));
  std::ranges::sort(visible.intersections, [](auto a, auto b) {
    return (a.right - a.left) * (a.bottom - a.top) > (b.right - b.left) * (b.bottom - b.top);
  });
  for (auto intersection : visible.intersections) {
    // 取滚动区域右侧空白，不回退到卡片热区造成悬停变化。
    LONG x = std::max(intersection.left, intersection.right - 2);
    for (double fraction : {0.5, 0.25, 0.75, 0.1, 0.9}) {
      LONG y = intersection.top +
               static_cast<LONG>((intersection.bottom - intersection.top - 1) * fraction);
      if (GetAncestor(WindowFromPoint({x, y}), GA_ROOT) == target.hwnd) {
        return POINT{x - size->origin.x, y - size->origin.y};
      }
    }
  }
  return std::unexpected(core::error("error.window.scroll_region_obscured"));
}

auto position_pointer(core::AppState& state, const Target& target, POINT point)
    -> std::expected<void, core::Error> {
  if (auto ok = check(state, target); !ok) {
    return ok;
  }
  auto size = geometry(target.hwnd);
  if (!size) {
    return std::unexpected(size.error());
  }
  if (point.x < 0 || point.y < 0 || point.x >= target.width || point.y >= target.height) {
    return std::unexpected(core::error("error.window.point_outside_client"));
  }
  POINT screen{size->origin.x + point.x, size->origin.y + point.y};
  if (!MonitorFromPoint(screen, MONITOR_DEFAULTTONULL) ||
      GetAncestor(WindowFromPoint(screen), GA_ROOT) != target.hwnd) {
    return std::unexpected(core::error("error.window.point_obscured"));
  }
  if (!SetCursorPos(screen.x, screen.y)) {
    return std::unexpected(error("window.operation.move_pointer"));
  }
  return {};
}

auto scroll(core::AppState& state, const Target& target, POINT point, int notches)
    -> std::expected<void, core::Error> {
  if (notches < 1 || notches > 273) {
    return std::unexpected(core::error("error.window.invalid_scroll_amount"));
  }
  if (auto ok = position_pointer(state, target, point); !ok) {
    return ok;
  }
  return wheel(state, target, notches);
}

auto wheel(core::AppState& state, const Target& target, int notches)
    -> std::expected<void, core::Error> {
  if (notches == 0 || notches < -273 || notches > 273) {
    return std::unexpected(core::error("error.window.invalid_wheel_amount"));
  }
  if (auto ok = check(state, target); !ok) {
    return ok;
  }
  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dwFlags = MOUSEEVENTF_WHEEL;
  input.mi.mouseData = static_cast<DWORD>(-WHEEL_DELTA * notches);
  if (SendInput(1, &input, sizeof(input)) != 1) {
    return std::unexpected(error("window.operation.send_input"));
  }
  return {};
}

auto click(core::AppState& state, const Target& target) -> std::expected<void, core::Error> {
  if (auto ok = check(state, target); !ok) {
    return ok;
  }
  INPUT inputs[2]{};
  for (auto& input : inputs) {
    input.type = INPUT_MOUSE;
  }
  inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
  inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
  auto count = SendInput(2, inputs, sizeof(INPUT));
  if (count == 2) {
    return {};
  }
  if (count == 1) {
    SendInput(1, &inputs[1], sizeof(INPUT));
  }
  return std::unexpected(error("window.operation.click"));
}

}  // namespace features::window

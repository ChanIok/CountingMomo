#include "features/window/window.hpp"

#include "vendor/std.hpp"

#include "vendor/windows/tlhelp32.hpp"

#include "cli.hpp"
#include "utils/string.hpp"

namespace features::window {
namespace {
std::atomic_bool cancelled{};
auto error(std::string_view operation) -> std::string {
  return std::format("{}失败，Win32={}；请确认与游戏权限一致。", operation, GetLastError());
}
auto title(HWND hwnd) -> std::wstring {
  std::wstring value(GetWindowTextLengthW(hwnd) + 1, L'\0');
  auto length = GetWindowTextW(hwnd, value.data(), static_cast<int>(value.size()));
  value.resize(length);
  return value;
}
struct Process {
  DWORD pid{};
  std::wstring exe;
};
// 进程名只做 ASCII 大小写折叠；游戏可执行文件名不含非 ASCII 字符。
auto ascii_lower(std::wstring_view text) -> std::wstring {
  std::wstring value(text);
  for (auto& c : value)
    if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
  return value;
}
// 单次快照供窗口列表标注进程名，也供按进程名定位，避免逐窗口 OpenProcess。
auto processes() -> std::vector<Process> {
  std::vector<Process> result;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return result;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snapshot, &entry))
    do {
      result.push_back({entry.th32ProcessID, entry.szExeFile});
    } while (Process32NextW(snapshot, &entry));
  CloseHandle(snapshot);
  return result;
}
struct Lookup {
  const std::vector<DWORD>* pids{};
  HWND hwnd{};
  LONG area{};
};
}  // namespace
auto request_stop() -> void { cancelled.store(true); }
auto stop_requested() -> bool {
  if (GetAsyncKeyState(VK_F8) & 0x8000) cancelled.store(true);
  return cancelled.load();
}
auto list_windows() -> std::vector<WindowInfo> {
  struct Context {
    const std::vector<Process>* processes{};
    std::vector<WindowInfo>* result{};
  };
  auto snapshot = processes();
  std::vector<WindowInfo> result;
  Context context{&snapshot, &result};
  EnumWindows(
      [](HWND hwnd, LPARAM data) -> BOOL {
        auto& holder = *reinterpret_cast<Context*>(data);
        auto text = title(hwnd);
        if (!IsWindowVisible(hwnd) || text.empty()) return TRUE;
        DWORD pid{};
        GetWindowThreadProcessId(hwnd, &pid);
        std::wstring exe;
        for (const auto& process : *holder.processes)
          if (process.pid == pid) {
            exe = process.exe;
            break;
          }
        holder.result->push_back(
            {hwnd, pid, utils::string::utf8(exe), utils::string::utf8(text)});
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&context));
  return result;
}
auto find(HWND explicit_handle, std::wstring_view exe) -> std::expected<HWND, std::string> {
  if (explicit_handle) {
    if (!IsWindow(explicit_handle)) return std::unexpected("窗口句柄无效。");
    return explicit_handle;
  }
  auto wanted = ascii_lower(exe);
  if (wanted.empty()) return std::unexpected("进程名不能为空。");
  std::vector<DWORD> pids;
  for (const auto& process : processes())
    if (ascii_lower(process.exe) == wanted) pids.push_back(process.pid);
  if (pids.empty())
    return std::unexpected(std::format("未找到进程 {}；请先启动游戏。", utils::string::utf8(exe)));
  Lookup lookup{&pids};
  // 游戏会创建额外的顶层辅助窗口，主窗口取可见、无 owner、面积最大者。
  EnumWindows(
      [](HWND hwnd, LPARAM data) -> BOOL {
        auto& holder = *reinterpret_cast<Lookup*>(data);
        DWORD pid{};
        GetWindowThreadProcessId(hwnd, &pid);
        if (std::ranges::find(*holder.pids, pid) == holder.pids->end()) return TRUE;
        if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
        // 最小化时 GetWindowRect 落在屏幕外，面积不可比，改用还原矩形。
        RECT rect{};
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        if (IsIconic(hwnd)) {
          if (!GetWindowPlacement(hwnd, &placement)) return TRUE;
          rect = placement.rcNormalPosition;
        } else if (!GetWindowRect(hwnd, &rect))
          return TRUE;
        auto area = (rect.right - rect.left) * (rect.bottom - rect.top);
        if (area > holder.area) {
          holder.area = area;
          holder.hwnd = hwnd;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&lookup));
  if (!lookup.hwnd)
    return std::unexpected(std::format(
        "进程 {} 已运行，但没有可用的顶层窗口；请进入游戏主界面后重试。", utils::string::utf8(exe)));
  return lookup.hwnd;
}
auto geometry(HWND hwnd) -> std::expected<Geometry, std::string> {
  RECT r{};
  POINT p{};
  if (!IsWindow(hwnd) || IsIconic(hwnd)) return std::unexpected("游戏窗口已关闭或最小化。");
  if (!GetClientRect(hwnd, &r) || !ClientToScreen(hwnd, &p))
    return std::unexpected(error("读取客户区"));
  return Geometry{r.right, r.bottom, p};
}
auto prepare(HWND hwnd, int width, int height, double timeout)
    -> std::expected<Target, std::string> {
  if (stop_requested()) return std::unexpected("已停止。");
  using Clock = std::chrono::steady_clock;
  auto deadline = Clock::now() + std::chrono::duration<double>(timeout);
  if (IsIconic(hwnd) || IsZoomed(hwnd)) ShowWindowAsync(hwnd, SW_RESTORE);
  while (IsIconic(hwnd) || IsZoomed(hwnd)) {
    if (stop_requested()) return std::unexpected("已停止。");
    if (Clock::now() >= deadline) return std::unexpected("等待窗口恢复超时。");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  MONITORINFO monitor{sizeof(MONITORINFO)};
  if (!GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor))
    return std::unexpected(error("读取主屏"));
  auto x = monitor.rcMonitor.left + (monitor.rcMonitor.right - monitor.rcMonitor.left - width) / 2;
  auto y = monitor.rcMonitor.top;
  SetLastError(0);
  auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  if (!style && GetLastError()) return std::unexpected(error("读取窗口样式"));
  SetLastError(0);
  if (!SetWindowLongPtrW(hwnd, GWL_STYLE,
                         (style & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW)) | WS_POPUP) &&
      GetLastError())
    return std::unexpected(error("设置无边框"));
  if (!SetWindowPos(hwnd, nullptr, x, y, width, height,
                    SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS))
    return std::unexpected(error("设置窗口尺寸"));
  std::optional<Clock::time_point> since;
  while (Clock::now() < deadline) {
    if (stop_requested()) return std::unexpected("已停止。");
    auto actual = geometry(hwnd);
    if (!actual) return std::unexpected(actual.error());
    auto now = Clock::now();
    if (actual->width == width && actual->height == height && actual->origin.x == x &&
        actual->origin.y == y) {
      if (!since) since = now;
      if (now - *since >= std::chrono::milliseconds(250)) {
        DWORD pid{};
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid) return std::unexpected("游戏进程已退出。");
        cli::log(std::format("已设置无边框竖屏 {}×{}，位置 ({}, {})。", width, height, x, y));
        return Target{hwnd, pid, width, height};
      }
    } else
      since.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  return std::unexpected("设置窗口尺寸超时；请确认游戏处于窗口模式且权限一致。");
}
auto check(const Target& target, bool foreground) -> std::expected<void, std::string> {
  if (stop_requested()) return std::unexpected("F8 / Ctrl+C：已停止采集。");
  DWORD pid{};
  GetWindowThreadProcessId(target.hwnd, &pid);
  if (pid != target.pid) return std::unexpected("目标游戏进程已变化。");
  auto size = geometry(target.hwnd);
  if (!size) return std::unexpected(size.error());
  if (size->width != target.width || size->height != target.height)
    return std::unexpected("扫描期间窗口尺寸发生变化。");
  if (foreground && GetForegroundWindow() != target.hwnd)
    return std::unexpected("游戏失去前台焦点，停止采集。");
  return {};
}
auto activate(const Target& target) -> std::expected<void, std::string> {
  using Clock = std::chrono::steady_clock;
  if (auto ok = check(target, false); !ok) return ok;
  cli::log("正在激活游戏；若未自动切换，请在30秒内 Alt+Tab 切回游戏。F8 取消。");
  SetForegroundWindow(target.hwnd);
  auto deadline = Clock::now() + std::chrono::seconds(30);
  std::optional<Clock::time_point> since;
  while (Clock::now() < deadline) {
    if (auto ok = check(target, false); !ok) return ok;
    if (GetForegroundWindow() == target.hwnd) {
      if (!since) since = Clock::now();
      if (Clock::now() - *since >= std::chrono::milliseconds(200)) return {};
    } else
      since.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  return std::unexpected("等待游戏前台超时。");
}
auto scroll_point(const Target& target, const scan::Box& bounds)
    -> std::expected<POINT, std::string> {
  auto size = geometry(target.hwnd);
  if (!size) return std::unexpected(size.error());
  if (bounds[0] < 0 || bounds[1] < 0 || bounds[2] > target.width || bounds[3] > target.height ||
      bounds[0] >= bounds[2] || bounds[1] >= bounds[3])
    return std::unexpected("滚动区域无效。");
  struct Visible {
    RECT region;
    std::vector<RECT> intersections;
  };
  Visible v{{bounds[0] + size->origin.x, bounds[1] + size->origin.y, bounds[2] + size->origin.x,
             bounds[3] + size->origin.y},
            {}};
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR, HDC, LPRECT screen, LPARAM data) -> BOOL {
        auto& value = *reinterpret_cast<Visible*>(data);
        RECT overlap{};
        if (IntersectRect(&overlap, &value.region, screen)) value.intersections.push_back(overlap);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&v));
  std::ranges::sort(v.intersections, [](auto a, auto b) {
    return (a.right - a.left) * (a.bottom - a.top) > (b.right - b.left) * (b.bottom - b.top);
  });
  for (auto r : v.intersections) {
    // 取滚动区域右侧空白，不回退到卡片热区造成悬停变化。
    LONG x = std::max(r.left, r.right - 2);
    for (double fraction : {0.5, 0.25, 0.75, 0.1, 0.9}) {
      LONG y = r.top + static_cast<LONG>((r.bottom - r.top - 1) * fraction);
      if (GetAncestor(WindowFromPoint({x, y}), GA_ROOT) == target.hwnd)
        return POINT{x - size->origin.x, y - size->origin.y};
    }
  }
  return std::unexpected("列表右侧可见区域被遮挡；请露出游戏或指定 --scroll-point。");
}
auto position_pointer(const Target& target, POINT point) -> std::expected<void, std::string> {
  if (auto ok = check(target); !ok) return ok;
  auto size = geometry(target.hwnd);
  if (!size) return std::unexpected(size.error());
  if (point.x < 0 || point.y < 0 || point.x >= target.width || point.y >= target.height)
    return std::unexpected("滚动点超出客户区。");
  POINT screen{size->origin.x + point.x, size->origin.y + point.y};
  if (!MonitorFromPoint(screen, MONITOR_DEFAULTTONULL) ||
      GetAncestor(WindowFromPoint(screen), GA_ROOT) != target.hwnd)
    return std::unexpected("滚动点在显示器外或被遮挡。");
  if (!SetCursorPos(screen.x, screen.y)) return std::unexpected(error("移动鼠标"));
  return {};
}
auto scroll(const Target& target, POINT point, int notches) -> std::expected<void, std::string> {
  if (notches < 1 || notches > 273) return std::unexpected("滚轮幅度必须为1～273。");
  if (auto ok = position_pointer(target, point); !ok) return ok;
  if (auto ok = check(target); !ok) return ok;
  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dwFlags = MOUSEEVENTF_WHEEL;
  input.mi.mouseData = static_cast<DWORD>(-WHEEL_DELTA * notches);
  if (SendInput(1, &input, sizeof(input)) != 1) return std::unexpected(error("SendInput"));
  return {};
}
}  // namespace features::window

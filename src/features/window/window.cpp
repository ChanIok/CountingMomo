#include "features/window/window.hpp"

#include "vendor/std.hpp"

#include "vendor/windows/tlhelp32.hpp"

#include "core/message/message.hpp"
#include "core/state/app_state.hpp"
#include "features/scan/control.hpp"
#include "features/window/state.hpp"
#include "utils/string.hpp"

namespace features::window {

namespace {

// 构造窗口操作失败错误信息
auto error(std::string_view operation) -> core::Error {
  auto code = GetLastError();
  return core::error("error.window.operation_failed", core::text_key(std::string(operation)), code);
}

struct Process {
  DWORD pid{};
  std::wstring exe;
};

// 将字符串转换为 ASCII 小写
auto ascii_lower(std::wstring_view text) -> std::wstring {
  std::wstring value(text);
  for (auto& c : value) {
    if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c - L'A' + L'a');
    }
  }
  return value;
}

// 获取当前系统全部进程快照：创建快照句柄 → 遍历进程列表 → 提取 PID 与程序名
auto processes() -> std::vector<Process> {
  std::vector<Process> result;
  // 获取当前系统所有运行进程快照
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return result;
  }
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  // 遍历进程链表收集进程 ID 和可执行文件名
  if (Process32FirstW(snapshot, &entry)) {
    do {
      result.push_back({entry.th32ProcessID, entry.szExeFile});
    } while (Process32NextW(snapshot, &entry));
  }
  // 关闭快照句柄
  CloseHandle(snapshot);
  return result;
}

struct Lookup {
  const std::vector<DWORD>* pids{};
  HWND hwnd{};
  LONG area{};
};

}  // namespace

// 恢复游戏原窗口样式与尺寸：校验窗口有效性 → 还原普通与扩展样式 → 还原位置与最大化状态
auto restore(core::AppState& state) -> std::expected<void, core::Error> {
  auto& original = state.window->original;
  if (!original) {
    return {};
  }
  // 校验目标窗口当前进程是否匹配，避免将样式应用到句柄复用的无关进程
  DWORD pid{};
  GetWindowThreadProcessId(original->hwnd, &pid);
  if (!IsWindow(original->hwnd) || pid != original->pid) {
    original.reset();
    return {};  // 原窗口已销毁，不能把旧样式写给复用句柄的新窗口。
  }
  SetLastError(0);
  // 还原普通窗口样式（边框、标题栏等）
  if (!SetWindowLongPtrW(original->hwnd, GWL_STYLE, original->style) && GetLastError()) {
    return std::unexpected(error("window.operation.restore_style"));
  }
  SetLastError(0);
  // 还原扩展窗口样式
  if (!SetWindowLongPtrW(original->hwnd, GWL_EXSTYLE, original->ex_style) && GetLastError()) {
    return std::unexpected(error("window.operation.restore_ex_style"));
  }
  // 刷新窗口非客户区框架并恢复先前的窗口位置与显示状态
  if (!SetWindowPos(original->hwnd, nullptr, 0, 0, 0, 0,
                    SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) ||
      !SetWindowPlacement(original->hwnd, &original->placement)) {
    return std::unexpected(error("window.operation.restore_position"));
  }
  // 清理备份状态
  original.reset();
  return {};
}

// 查找游戏顶层主窗口：按 exe 筛选 PID → 遍历顶层窗口 → 选取面积最大的无 Owner 可见窗口
auto find(HWND explicit_handle, std::wstring_view exe) -> std::expected<HWND, core::Error> {
  // 如果调用者显式传入了窗口句柄，直接校验并返回
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
  // 查找目标游戏进程名称匹配的所有 PID
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
  // 检查是否成功定位主窗口
  if (!lookup.hwnd) {
    return std::unexpected(core::error("error.window.no_main_window", utils::string::utf8(exe)));
  }
  return lookup.hwnd;
}

// 获取窗口客户区几何信息：检查窗口有效性 → 获取客户区尺寸并转换为屏幕绝对原点
auto geometry(HWND hwnd) -> std::expected<Geometry, core::Error> {
  RECT client_rect{};
  POINT origin{};
  // 确保窗口有效且未处于最小化状态
  if (!IsWindow(hwnd) || IsIconic(hwnd)) {
    return std::unexpected(core::error("error.window.unavailable"));
  }
  // 获取客户区矩形并换算原点到屏幕坐标
  if (!GetClientRect(hwnd, &client_rect) || !ClientToScreen(hwnd, &origin)) {
    return std::unexpected(error("window.operation.read_client"));
  }
  return Geometry{client_rect.right, client_rect.bottom, origin};
}

// 调整目标窗口为竖屏无边框：保存原窗口状态 → 还原最小化 → 居中计算 → 去边框改 POPUP →
// 轮询等待几何稳定
auto prepare(core::AppState& state, HWND hwnd, int width, int height, double timeout)
    -> std::expected<Target, core::Error> {
  // 检查是否已收到停止信号
  if (scan::stop_requested(state)) {
    return std::unexpected(core::error("error.scan.stopped"));
  }
  // 首次准备时备份原始样式和位置，以便扫描结束后恢复
  if (!state.window->original) {
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
  // 若窗口处于最小化或最大化状态，异步发送还原指令
  if (IsIconic(hwnd) || IsZoomed(hwnd)) {
    ShowWindowAsync(hwnd, SW_RESTORE);
  }
  // 等待还原生效
  while (IsIconic(hwnd) || IsZoomed(hwnd)) {
    if (scan::stop_requested(state)) {
      return std::unexpected(core::error("error.scan.stopped"));
    }
    if (Clock::now() >= deadline) {
      return std::unexpected(core::error("error.window.restore_timeout"));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  // 读取主显示器边界，将竖屏目标居中摆放
  MONITORINFO monitor{sizeof(MONITORINFO)};
  if (!GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor)) {
    return std::unexpected(error("window.operation.read_monitor"));
  }
  auto x = monitor.rcMonitor.left + (monitor.rcMonitor.right - monitor.rcMonitor.left - width) / 2;
  auto y = monitor.rcMonitor.top;
  SetLastError(0);
  // 移除标准重叠窗口边框，改设为 POPUP 样式
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
  // 异步调整窗口物理位置与高宽
  if (!SetWindowPos(hwnd, nullptr, x, y, width, height,
                    SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS)) {
    return std::unexpected(error("window.operation.resize"));
  }
  std::optional<Clock::time_point> since;
  // 轮询检查实际客户区是否精确对齐并在 250ms 内保持稳定
  while (Clock::now() < deadline) {
    if (scan::stop_requested(state)) {
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
        return Target{hwnd, pid, width, height};
      }
    } else {
      since.reset();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  return std::unexpected(core::error("error.window.resize_timeout"));
}

// 运行时校验目标窗口状态：检查停止标志 → 检查 PID 匹配 → 检查客户区尺寸 → 按需检查前台焦点
auto check(core::AppState& state, const Target& target, bool foreground)
    -> std::expected<void, core::Error> {
  // 检查是否已收到停止信号
  if (scan::stop_requested(state)) {
    return std::unexpected(core::error("error.scan.cancelled"));
  }
  // 校验窗口对应进程是否仍然存活且一致
  DWORD pid{};
  GetWindowThreadProcessId(target.hwnd, &pid);
  if (pid != target.pid) {
    return std::unexpected(core::error("error.window.process_changed"));
  }
  // 校验窗口客户区物理高宽是否被用户意外调整
  auto size = geometry(target.hwnd);
  if (!size) {
    return std::unexpected(size.error());
  }
  if (size->width != target.width || size->height != target.height) {
    return std::unexpected(core::error("error.window.size_changed"));
  }
  // 按需校验游戏窗口是否依然持有前台焦点
  if (foreground && GetForegroundWindow() != target.hwnd) {
    return std::unexpected(core::error("error.window.focus_lost"));
  }
  return {};
}

// 激活目标窗口并等待前台焦点：调用 SetForegroundWindow → 轮询等待获得焦点并稳定持续
auto activate(core::AppState& state, const Target& target) -> std::expected<void, core::Error> {
  using Clock = std::chrono::steady_clock;
  // 基础环境校验
  if (auto ok = check(state, target, false); !ok) {
    return ok;
  }
  // 尝试拉起目标游戏窗口到前台
  SetForegroundWindow(target.hwnd);
  auto deadline = Clock::now() + std::chrono::seconds(30);
  std::optional<Clock::time_point> since;
  // 轮询等待窗口真正获取前台焦点且维持稳定至少 200ms
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

// 计算安全滚轮输入点：计算显示器可见重叠交集 → 选择右侧空白避开卡片热区 → 验证点命中目标窗口
auto scroll_point(const Target& target, const scan::Box& bounds)
    -> std::expected<POINT, core::Error> {
  auto size = geometry(target.hwnd);
  if (!size) {
    return std::unexpected(size.error());
  }
  // 校验滚动区域参数是否合法
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
  // 遍历所有显示器，计算目标滚动区域与物理屏幕的交集矩形
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
  // 优先选取面积最大的可见重叠区域
  std::ranges::sort(visible.intersections, [](auto a, auto b) {
    return (a.right - a.left) * (a.bottom - a.top) > (b.right - b.left) * (b.bottom - b.top);
  });
  // 在交集区域寻找无遮挡的输入坐标点
  for (auto intersection : visible.intersections) {
    // 取滚动区域右侧空白，不回退到卡片热区造成悬停变化。
    LONG x = std::max(intersection.left, intersection.right - 2);
    // 按不同垂直分位比例尝试探测有效输入点
    for (double fraction : {0.5, 0.25, 0.75, 0.1, 0.9}) {
      LONG y = intersection.top +
               static_cast<LONG>((intersection.bottom - intersection.top - 1) * fraction);
      // 确认该屏幕坐标点上的顶层窗口确实属于目标游戏窗口
      if (GetAncestor(WindowFromPoint({x, y}), GA_ROOT) == target.hwnd) {
        return POINT{x - size->origin.x, y - size->origin.y};
      }
    }
  }
  return std::unexpected(core::error("error.window.scroll_region_obscured"));
}

// 移动物理鼠标光标到目标客户区坐标：校验前台与有效性 → 验证屏幕点无遮挡 → 调用 SetCursorPos
auto position_pointer(core::AppState& state, const Target& target, POINT point)
    -> std::expected<void, core::Error> {
  // 校验目标窗口前台与有效性
  if (auto ok = check(state, target); !ok) {
    return ok;
  }
  auto size = geometry(target.hwnd);
  if (!size) {
    return std::unexpected(size.error());
  }
  // 校验坐标在客户区内部
  if (point.x < 0 || point.y < 0 || point.x >= target.width || point.y >= target.height) {
    return std::unexpected(core::error("error.window.point_outside_client"));
  }
  POINT screen{size->origin.x + point.x, size->origin.y + point.y};
  // 确认目标物理点在显示器内且未被其他窗口遮挡
  if (!MonitorFromPoint(screen, MONITOR_DEFAULTTONULL) ||
      GetAncestor(WindowFromPoint(screen), GA_ROOT) != target.hwnd) {
    return std::unexpected(core::error("error.window.point_obscured"));
  }
  // 移动系统光标到屏幕物理坐标
  if (!SetCursorPos(screen.x, screen.y)) {
    return std::unexpected(error("window.operation.move_pointer"));
  }
  return {};
}

// 在指定客户区坐标发送鼠标滚轮事件：移动光标位置 → 发送 WHEEL 事件
auto scroll(core::AppState& state, const Target& target, POINT point, int notches)
    -> std::expected<void, core::Error> {
  // 校验滚轮格数范围
  if (notches < 1 || notches > max_wheel_notches) {
    return std::unexpected(core::error("error.window.invalid_scroll_amount"));
  }
  // 先把鼠标光标定位到安全的空白区域
  if (auto ok = position_pointer(state, target, point); !ok) {
    return ok;
  }
  // 注入滚轮向下滚动输入
  return wheel(state, target, notches);
}

// 在当前光标位置注入鼠标滚轮输入：校验前台状态 → 构造 MOUSEINPUT 事件并发送
auto wheel(core::AppState& state, const Target& target, int notches)
    -> std::expected<void, core::Error> {
  // 校验滚轮格数
  if (notches == 0 || notches < -max_wheel_notches || notches > max_wheel_notches) {
    return std::unexpected(core::error("error.window.invalid_wheel_amount"));
  }
  // 校验目标窗口前台焦点
  if (auto ok = check(state, target); !ok) {
    return ok;
  }
  // 构造物理鼠标滚轮输入事件（负值向下滚动）
  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dwFlags = MOUSEEVENTF_WHEEL;
  input.mi.mouseData = static_cast<DWORD>(-WHEEL_DELTA * notches);
  // 注入物理输入
  if (SendInput(1, &input, sizeof(input)) != 1) {
    return std::unexpected(error("window.operation.send_input"));
  }
  return {};
}

// 模拟鼠标左键点击事件：构造并发送按下与抬起输入序列
auto click(core::AppState& state, const Target& target) -> std::expected<void, core::Error> {
  // 校验前台焦点
  if (auto ok = check(state, target); !ok) {
    return ok;
  }
  // 构造鼠标左键按下和抬起的完整点击输入序列
  INPUT inputs[2]{};
  for (auto& input : inputs) {
    input.type = INPUT_MOUSE;
  }
  inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
  inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
  // 发送鼠标输入序列
  auto count = SendInput(2, inputs, sizeof(INPUT));
  if (count == 2) {
    return {};
  }
  // 若只成功发送了按下事件，补发抬起事件以避免按键挂起
  if (count == 1) {
    SendInput(1, &inputs[1], sizeof(INPUT));
  }
  return std::unexpected(error("window.operation.click"));
}

}  // namespace features::window

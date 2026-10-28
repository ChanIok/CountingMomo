"""Win32 窗口定位、物理像素坐标和 SendInput 滚轮输入。"""

import ctypes
from ctypes import wintypes
from time import monotonic, sleep

from scrolling import MAX_WHEEL_NOTCHES


user32 = ctypes.WinDLL("user32", use_last_error=True)
dwmapi = ctypes.WinDLL("dwmapi", use_last_error=True)
enum_proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
monitor_enum_proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HANDLE, wintypes.HDC,
                                      ctypes.POINTER(wintypes.RECT), wintypes.LPARAM)


class MonitorInfo(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", wintypes.RECT),
                ("rcWork", wintypes.RECT), ("dwFlags", wintypes.DWORD)]


class MouseInput(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG),
                ("mouseData", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class InputUnion(ctypes.Union):
    _fields_ = [("mi", MouseInput)]


class Input(ctypes.Structure):
    _anonymous_ = ("data",)
    _fields_ = [("type", wintypes.DWORD), ("data", InputUnion)]


def bind(name, args, result):
    fn = getattr(user32, name)
    fn.argtypes, fn.restype = args, result
    return fn


bind("EnumWindows", [enum_proc, wintypes.LPARAM], wintypes.BOOL)
bind("EnumDisplayMonitors", [wintypes.HDC, ctypes.POINTER(wintypes.RECT), monitor_enum_proc, wintypes.LPARAM], wintypes.BOOL)
bind("GetWindowTextLengthW", [wintypes.HWND], ctypes.c_int)
bind("GetWindowTextW", [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int], ctypes.c_int)
bind("IsWindowVisible", [wintypes.HWND], wintypes.BOOL)
bind("IsWindow", [wintypes.HWND], wintypes.BOOL)
bind("IsIconic", [wintypes.HWND], wintypes.BOOL)
bind("IsZoomed", [wintypes.HWND], wintypes.BOOL)
bind("ShowWindowAsync", [wintypes.HWND, ctypes.c_int], wintypes.BOOL)
bind("GetWindowLongW", [wintypes.HWND, ctypes.c_int], wintypes.LONG)
bind("SetWindowLongW", [wintypes.HWND, ctypes.c_int, wintypes.LONG], wintypes.LONG)
bind("SetWindowPos", [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
                      ctypes.c_int, ctypes.c_int, wintypes.UINT], wintypes.BOOL)
bind("GetMonitorInfoW", [wintypes.HANDLE, ctypes.POINTER(MonitorInfo)], wintypes.BOOL)
bind("GetClientRect", [wintypes.HWND, ctypes.POINTER(wintypes.RECT)], wintypes.BOOL)
bind("GetWindowRect", [wintypes.HWND, ctypes.POINTER(wintypes.RECT)], wintypes.BOOL)
bind("ClientToScreen", [wintypes.HWND, ctypes.POINTER(wintypes.POINT)], wintypes.BOOL)
bind("GetForegroundWindow", [], wintypes.HWND)
bind("SetForegroundWindow", [wintypes.HWND], wintypes.BOOL)
bind("SetCursorPos", [ctypes.c_int, ctypes.c_int], wintypes.BOOL)
bind("WindowFromPoint", [wintypes.POINT], wintypes.HWND)
bind("MonitorFromPoint", [wintypes.POINT, wintypes.DWORD], wintypes.HANDLE)
bind("GetAncestor", [wintypes.HWND, wintypes.UINT], wintypes.HWND)
bind("GetAsyncKeyState", [ctypes.c_int], ctypes.c_short)
bind("GetWindowThreadProcessId", [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)], wintypes.DWORD)
bind("SendInput", [wintypes.UINT, ctypes.POINTER(Input), ctypes.c_int], wintypes.UINT)
bind("SetProcessDpiAwarenessContext", [wintypes.HANDLE], wintypes.BOOL)
dwmapi.DwmGetWindowAttribute.argtypes = [wintypes.HWND, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
dwmapi.DwmGetWindowAttribute.restype = ctypes.c_long


def enable_physical_coordinates():
    if not user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4)):
        if ctypes.get_last_error() != 5:  # 已设定进程 DPI 感知时无需重设。
            raise ctypes.WinError(ctypes.get_last_error())


def window_title(hwnd):
    text = ctypes.create_unicode_buffer(user32.GetWindowTextLengthW(hwnd) + 1)
    user32.GetWindowTextW(hwnd, text, len(text))
    return text.value


def list_windows():
    windows = []

    @enum_proc
    def visit(hwnd, _):
        if user32.IsWindowVisible(hwnd) and (title := window_title(hwnd)):
            windows.append({"hwnd": int(hwnd), "title": title})
        return True

    if not user32.EnumWindows(visit, 0):
        raise ctypes.WinError(ctypes.get_last_error())
    return windows


def find_window(hwnd=None, title="无限暖暖"):
    if hwnd is not None:
        if not user32.IsWindow(hwnd):
            raise ValueError(f"窗口句柄无效：{hwnd:#x}")
        return hwnd
    matches = [w for w in list_windows() if title.casefold() in w["title"].casefold()]
    if len(matches) != 1:
        raise ValueError(f"窗口标题 {title!r} 匹配到 {len(matches)} 个窗口，请使用 --list-windows 后指定 --hwnd。")
    return matches[0]["hwnd"]


def window_pid(hwnd):
    pid = wintypes.DWORD()
    if not user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid)):
        raise RuntimeError("目标窗口已关闭。")
    return pid.value


def client_geometry(hwnd):
    if not user32.IsWindow(hwnd) or user32.IsIconic(hwnd):
        raise RuntimeError("目标窗口已关闭或最小化。")
    rect, origin = wintypes.RECT(), wintypes.POINT(0, 0)
    if not user32.GetClientRect(hwnd, ctypes.byref(rect)) or not user32.ClientToScreen(hwnd, ctypes.byref(origin)):
        raise ctypes.WinError(ctypes.get_last_error())
    return (rect.right, rect.bottom), (origin.x, origin.y)


def prepare_scan_window(hwnd, size, check_stop, timeout=8.0):
    """无边框竖屏，主屏水平居中、顶边贴齐；完成后才允许创建 WGC。"""
    check_stop()
    deadline = monotonic() + timeout
    # 最大化状态会限制窗口矩形，先退出最大化/最小化再设置扫描尺寸。
    if user32.IsIconic(hwnd) or user32.IsZoomed(hwnd):
        if not user32.ShowWindowAsync(hwnd, 9):  # SW_RESTORE
            raise ctypes.WinError(ctypes.get_last_error())
        while user32.IsIconic(hwnd) or user32.IsZoomed(hwnd):
            check_stop()
            if monotonic() >= deadline:
                raise RuntimeError("等待游戏退出最小化/最大化状态超时。")
            sleep(0.05)

    # 原点位于主屏；使用整块屏幕而不是避开任务栏的工作区。
    monitor = user32.MonitorFromPoint(wintypes.POINT(0, 0), 1)
    info = MonitorInfo(cbSize=ctypes.sizeof(MonitorInfo))
    if not monitor or not user32.GetMonitorInfoW(monitor, ctypes.byref(info)):
        raise ctypes.WinError(ctypes.get_last_error())
    bounds = info.rcMonitor
    width, height = size
    x = bounds.left + (bounds.right - bounds.left - width) // 2
    y = bounds.top

    ctypes.set_last_error(0)
    style = user32.GetWindowLongW(hwnd, -16)  # GWL_STYLE
    if style == 0 and ctypes.get_last_error():
        raise ctypes.WinError(ctypes.get_last_error())
    # 与 SpinningMomo resize_and_center_window 相同：移除标准边框，改为弹出窗口。
    borderless = ((style & 0xFFFFFFFF) & ~0x00CF0000) | 0x80000000  # ~WS_OVERLAPPEDWINDOW | WS_POPUP
    if (style & 0xFFFFFFFF) != borderless:
        ctypes.set_last_error(0)
        previous = user32.SetWindowLongW(hwnd, -16, ctypes.c_long(borderless).value)
        if previous == 0 and ctypes.get_last_error():
            raise ctypes.WinError(ctypes.get_last_error())
    # FRAMECHANGED 应用样式；异步跨线程提交，等待实际几何尺寸而非仅相信返回值。
    flags = 0x0020 | 0x0004 | 0x0010 | 0x4000  # FRAMECHANGED | NOZORDER | NOACTIVATE | ASYNCWINDOWPOS
    if not user32.SetWindowPos(hwnd, None, x, y, width, height, flags):
        raise ctypes.WinError(ctypes.get_last_error())
    stable_since = None
    actual = None
    while monotonic() < deadline:
        check_stop()
        actual = client_geometry(hwnd)
        now = monotonic()
        if actual == (tuple(size), (x, y)):
            if stable_since is None:
                stable_since = now
            if now - stable_since >= 0.25:
                print(f"已设置无边框竖屏 {width}×{height}，位置 ({x}, {y})；顶边已对齐主屏。", flush=True)
                return {"client_size": list(size), "client_origin": [x, y], "borderless": True,
                        "primary_monitor": [bounds.left, bounds.top, bounds.right, bounds.bottom]}
        else:
            stable_since = None
        sleep(0.05)
    raise RuntimeError(f"设置无边框竖屏超时：期望客户区 {size}、位置 ({x}, {y})，实际 {actual}。请确认游戏为窗口模式，且终端与游戏权限一致。")


def capture_client_box(hwnd, frame_size):
    size, origin = client_geometry(hwnd)
    if tuple(frame_size) == size:
        return (0, 0, *size)
    # WGC 的窗口帧通常以可见外框为原点；有标题栏时裁去非客户区。
    bounds = wintypes.RECT()
    candidates = []
    if dwmapi.DwmGetWindowAttribute(hwnd, 9, ctypes.byref(bounds), ctypes.sizeof(bounds)) == 0:
        candidates.append((bounds.left, bounds.top, bounds.right, bounds.bottom))
    if user32.GetWindowRect(hwnd, ctypes.byref(bounds)):
        candidates.append((bounds.left, bounds.top, bounds.right, bounds.bottom))
    for left, top, right, bottom in candidates:
        if (right - left, bottom - top) != tuple(frame_size):
            continue
        x, y = origin[0] - left, origin[1] - top
        if x >= 0 and y >= 0 and x + size[0] <= frame_size[0] and y + size[1] <= frame_size[1]:
            return (x, y, x + size[0], y + size[1])
    raise RuntimeError(f"无法确定 WGC 帧 {frame_size} 中的客户区 {size}，请先用 --capture-only 检查。")


def activate(hwnd, check_stop, timeout=30.0):
    check_stop()
    print(f"字体模板已就绪，正在激活游戏；若未自动切换，请在 {timeout:g} 秒内 Alt+Tab 切回游戏。F8 取消。", flush=True)
    user32.SetForegroundWindow(hwnd)
    # 激活请求不等于焦点已切换；允许用户完成切换，不反复抢占前台。
    deadline = monotonic() + timeout
    foreground_since = None
    while monotonic() < deadline:
        check_stop()
        now = monotonic()
        if user32.GetForegroundWindow() == hwnd:
            if foreground_since is None:
                foreground_since = now
            if now - foreground_since >= 0.2:
                print("已确认游戏在前台，开始采集。", flush=True)
                return
        else:
            foreground_since = None
        sleep(0.05)
    foreground = user32.GetForegroundWindow()
    title = window_title(foreground) if foreground else "无前台窗口"
    raise RuntimeError(f"等待游戏前台超时（{timeout:g} 秒）；当前前台为 {title!r}。请在提示后 Alt+Tab 切回游戏。")


def stop_pressed():
    return bool(user32.GetAsyncKeyState(0x77) & 0x8000)  # F8，无需游戏前台支持。


def choose_scroll_point(hwnd, bounds):
    size, origin = client_geometry(hwnd)
    left, top, right, bottom = bounds
    if not 0 <= left < right <= size[0] or not 0 <= top < bottom <= size[1]:
        raise ValueError("自动滚动区域必须在客户区内。")
    visible = []

    @monitor_enum_proc
    def visit(_monitor, _dc, rect, _data):
        monitor = rect.contents
        x1, y1 = max(left + origin[0], monitor.left), max(top + origin[1], monitor.top)
        x2, y2 = min(right + origin[0], monitor.right), min(bottom + origin[1], monitor.bottom)
        if x1 < x2 and y1 < y2:
            visible.append((x1, y1, x2, y2))
        return True

    if not user32.EnumDisplayMonitors(None, None, visit, 0):
        raise ctypes.WinError(ctypes.get_last_error())
    # 靠近右缘空白处，避免卡片悬停亮框改变用于定位的左侧边框。
    for x1, y1, x2, y2 in sorted(visible, key=lambda rect: (rect[2] - rect[0]) * (rect[3] - rect[1]), reverse=True):
        xs = [max(x1, x2 - 2), (x1 + x2) // 2,
              x1 + (x2 - x1) // 4, x1 + 3 * (x2 - x1) // 4, min(x2 - 1, x1 + 12)]
        ys = [min(y2 - 1, y1 + round((y2 - y1) * fraction)) for fraction in (0.5, 0.25, 0.75, 0.1, 0.9)]
        for x in xs:
            for y in ys:
                hit = user32.WindowFromPoint(wintypes.POINT(x, y))
                if user32.GetAncestor(hit, 2) == hwnd:
                    return x - origin[0], y - origin[1]
    raise RuntimeError(f"列表可见区域内未找到未遮挡的滚动点（客户区原点 {origin}）；请移动窗口露出部分列表，或用 --scroll-point 指定可见位置。")


def position_pointer(hwnd, pid, point, expected_size):
    if window_pid(hwnd) != pid or user32.GetForegroundWindow() != hwnd:
        raise RuntimeError("目标窗口失去前台焦点或发生变化，已停止滚动。")
    size, origin = client_geometry(hwnd)
    if size != expected_size:
        raise RuntimeError("扫描期间窗口尺寸发生变化。")
    x, y = point
    if not 0 <= x < size[0] or not 0 <= y < size[1]:
        raise ValueError("滚动位置超出客户区。")
    screen_point = wintypes.POINT(origin[0] + x, origin[1] + y)
    if not user32.MonitorFromPoint(screen_point, 0):
        raise RuntimeError("滚动位置不在任何显示器上，请调整窗口位置或 --scroll-point。")
    hit = user32.WindowFromPoint(screen_point)
    if user32.GetAncestor(hit, 2) != hwnd:
        raise RuntimeError("滚动位置被其他窗口遮挡，请调整窗口位置或 --scroll-point。")
    if not user32.SetCursorPos(screen_point.x, screen_point.y):
        raise ctypes.WinError(ctypes.get_last_error())


def scroll_down(hwnd, pid, point, notches, expected_size):
    if not 1 <= notches <= MAX_WHEEL_NOTCHES:
        raise ValueError(f"单次滚轮格数必须在 1–{MAX_WHEEL_NOTCHES} 内。")
    position_pointer(hwnd, pid, point, expected_size)
    packet = Input(type=0, mi=MouseInput(mouseData=(-120 * notches) & 0xFFFFFFFF, dwFlags=0x0800))
    if user32.GetForegroundWindow() != hwnd or stop_pressed():
        raise RuntimeError("发送滚轮输入前窗口失去焦点或按下了 F8。")
    if user32.SendInput(1, ctypes.byref(packet), ctypes.sizeof(Input)) != 1:
        raise RuntimeError("SendInput 失败，请确认游戏与 Python 的权限级别相同。")

#pragma once

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

#include "core/message/message.hpp"
#include "features/scan/types.hpp"
#include "features/window/types.hpp"

namespace core {
struct AppState;
}

namespace features::window {

inline constexpr std::wstring_view game_executable = L"X6Game-Win64-Shipping.exe";
// 单次滚轮输入保持在有符号 16 位增量范围内。
inline constexpr int max_wheel_notches = std::numeric_limits<short>::max() / WHEEL_DELTA;

auto restore(core::AppState& state) -> std::expected<void, core::Error>;

// 显式句柄优先；否则精确匹配 exe（ASCII 大小写无关），取可见无 owner 的最大窗口。
// 进程存在但没有可用窗口时直接失败，不轮询。
auto find(HWND explicit_handle, std::wstring_view exe) -> std::expected<HWND, core::Error>;
auto geometry(HWND hwnd) -> std::expected<Geometry, core::Error>;

// 调整为主屏水平居中、顶边对齐的无边框客户区，等待几何稳定；timeout 单位为秒。
auto prepare(core::AppState& state, HWND hwnd, int width, int height, double timeout)
    -> std::expected<Target, core::Error>;
auto activate(core::AppState& state, const Target& target) -> std::expected<void, core::Error>;

// 校验停止请求、进程身份和客户区尺寸；foreground 为 true 时失焦按正常停止处理。
auto check(core::AppState& state, const Target& target, bool foreground = true)
    -> std::expected<void, core::Error>;
auto scroll_point(const Target& target, const scan::Box& bounds)
    -> std::expected<POINT, core::Error>;
auto position_pointer(core::AppState& state, const Target& target, POINT point)
    -> std::expected<void, core::Error>;

// 移动到客户区坐标 point 后向下滚动，notches 为 1～max_wheel_notches。
auto scroll(core::AppState& state, const Target& target, POINT point, int notches)
    -> std::expected<void, core::Error>;

// 在当前鼠标位置发送滚轮输入：正数向下、负数向上，不负责移动鼠标。
auto wheel(core::AppState& state, const Target& target, int notches)
    -> std::expected<void, core::Error>;

auto click(core::AppState& state, const Target& target) -> std::expected<void, core::Error>;
}  // namespace features::window

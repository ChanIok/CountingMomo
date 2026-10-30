#pragma once

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

#include "features/scan/types.hpp"

namespace features::window {
struct WindowInfo {
  HWND hwnd{};
  DWORD pid{};
  std::string exe;
  std::string title;
};
struct Geometry {
  int width{};
  int height{};
  POINT origin{};
};
struct Target {
  HWND hwnd{};
  DWORD pid{};
  int width{};
  int height{};
};
auto request_stop() -> void;
auto stop_requested() -> bool;
auto list_windows() -> std::vector<WindowInfo>;
auto find(HWND explicit_handle, std::wstring_view exe) -> std::expected<HWND, std::string>;
auto geometry(HWND hwnd) -> std::expected<Geometry, std::string>;
auto prepare(HWND hwnd, int width, int height, double timeout)
    -> std::expected<Target, std::string>;
auto activate(const Target& target) -> std::expected<void, std::string>;
auto check(const Target& target, bool foreground = true) -> std::expected<void, std::string>;
auto scroll_point(const Target& target, const scan::Box& bounds)
    -> std::expected<POINT, std::string>;
auto position_pointer(const Target& target, POINT point) -> std::expected<void, std::string>;
auto scroll(const Target& target, POINT point, int notches) -> std::expected<void, std::string>;

// 菜单及回顶使用有符号格数：正数向下、负数向上；不改变原scan的正数接口。
auto wheel(const Target& target, int notches) -> std::expected<void, std::string>;

auto click(const Target& target) -> std::expected<void, std::string>;
}  // namespace features::window

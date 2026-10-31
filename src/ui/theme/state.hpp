#pragma once

#include "vendor/std.hpp"

#include "vendor/windows.hpp"
#include "vendor/windows/winrt/windows_foundation.hpp"
#include "vendor/windows/winrt/windows_ui_viewmanagement.hpp"

#include "ui/theme.hpp"

namespace ui::theme {
// 回调只借用独立的通知目标；销毁窗口前先清空句柄，阻止在途回调继续投递。
struct Notification {
  std::mutex mutex;
  HWND target{};
  UINT message{};
};

struct State {
  // 构造时不激活 WinRT，在完成 COM 初始化后创建。
  winrt::Windows::UI::ViewManagement::UISettings settings{nullptr};
  std::shared_ptr<Notification> notification;
  winrt::Windows::UI::ViewManagement::UISettings::ColorValuesChanged_revoker changed;
  Palette colors{};
};
}  // namespace ui::theme

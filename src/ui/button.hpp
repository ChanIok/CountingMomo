#pragma once

#include "vendor/std.hpp"

#include "core/message/message.hpp"
#include "ui/composition.hpp"
#include "ui/theme.hpp"

namespace ui::button {

enum class Action {
  none,
  primary,
  secondary,
  settings,
  back,
  details,
  diagnostic,
  save,
  theme,
  repository,
  language,
  copy,
  view
};
enum class Style { primary, secondary, quiet };

struct Spec {
  Action action{};
  std::wstring label;
  D2D1_RECT_F bounds{};  // 客户区 DIP；已经包含正文滚动偏移。
  D2D1_RECT_F clip{};    // 客户区 DIP；绘制和命中使用同一裁剪。
  Style style{Style::quiet};
  bool enabled{true};
  std::wstring icon;
};

struct Button {
  Spec spec;
  composition::Node root;
  composition::Layer base;
  composition::Layer hover;
  composition::Layer pressed;
  composition::Layer ink;
  UINT dpi{};
};

struct State {
  std::vector<Button> buttons;
  Action hovered{};
  Action pressed{};  // 非 none 表示按钮手势持有捕获；取消或丢失捕获时立即清空。
};

// 同步可见按钮集合；仅模型、主题或 DPI 改变时重画素材，不重启动画。
auto sync(core::AppState& app, State& state, HWND hwnd, IDCompositionVisual2* parent,
          std::vector<Spec> specs, const theme::Palette& colors, UINT dpi, bool repaint)
    -> std::expected<void, core::Error>;
// 返回处理结果由窗口统一提交。只有同一按钮内按下并松开才产生动作。
auto pointer(core::AppState& app, State& state, HWND hwnd, UINT message, LPARAM lparam, UINT dpi)
    -> std::expected<Action, core::Error>;
// 取消交互并更新仍在显示的按钮外观；窗口销毁时直接清空状态、释放捕获和图层。
auto cancel(core::AppState& app, State& state, HWND hwnd) -> std::expected<void, core::Error>;

}  // namespace ui::button

#include "ui/status_window/painter.hpp"

#include "vendor/std.hpp"

#include "core/i18n/i18n.hpp"
#include "core/message/message.hpp"
#include "core/state/app_state.hpp"
#include "ui/button.hpp"
#include "ui/drawing.hpp"
#include "ui/status_window/state.hpp"
#include "ui/theme.hpp"

namespace ui::status_window::painter {

// 渲染悬浮状态窗：绘制背景与提示 → 绘制并驱动呼吸状态圆点 → 同步复制/查看按钮并提交合成
auto paint(core::AppState& state) -> std::expected<void, core::Error> {
  auto& window = *state.status_window;
  // 确保悬浮窗 DirectComposition 宿主已就绪
  if (auto ready = composition::create(state, window.composition, window.hwnd); !ready) {
    return ready;
  }
  auto colors = theme::resolve(state);
  RECT client{};
  GetClientRect(window.hwnd, &client);
  float width = client.right * 96.0f / window.dpi;
  float height = client.bottom * 96.0f / window.dpi;
  auto title = D2D1::RectF(40, 14, width - 20, 42);
  constexpr float indicator_size = 8;
  // 微软雅黑字形的视觉中心略低于行框中心，圆点随标题行定位并向下校正 1 DIP。
  float indicator_top = (title.top + title.bottom - indicator_size) / 2 + 1;
  // 仅在明确标记 redraw 时执行卡片主体离屏重绘
  if (window.redraw) {
    auto painted =
        composition::draw(state, window.composition.content, static_cast<UINT>(client.right),
                          static_cast<UINT>(client.bottom), window.dpi, [&](auto* target, auto) {
                            target->Clear(window.material ? D2D1::ColorF(0, 0.0f) : colors.card);
                            auto background = colors.card;
                            background.a = window.material ? 0.72f : 1.0f;
                            // 填充半透明圆角卡片底色
                            drawing::fill(target, D2D1::RectF(0, 0, width, height), background, 8);
                            drawing::text(state, target, window.title, title, 15, colors.text);
                            drawing::text(state, target, window.detail,
                                          D2D1::RectF(20, 49, width - 20, 77), 13, colors.muted);
                            // 运行态显示前台保持提示
                            if (window.active) {
                              drawing::text(state, target, window.hint,
                                            D2D1::RectF(20, 87, width - 20, 118), 12, colors.muted);
                            }
                          });
    if (!painted) {
      return painted;
    }
  }
  // 挂载呼吸圆点 Visual 节点
  if (!window.indicator.node.visual) {
    if (auto ready =
            composition::attach(state, window.indicator.node, window.composition.root.visual.Get());
        !ready) {
      return ready;
    }
  }
  // 绘制状态圆点
  if (window.redraw || window.repaint_buttons || !window.indicator.surface) {
    UINT size = static_cast<UINT>(std::ceil(indicator_size * window.dpi / 96.0f));
    if (auto ready = composition::draw(
            state, window.indicator, size, size, window.dpi,
            [&](auto* target, auto) {
              drawing::fill(target, D2D1::RectF(0, 0, indicator_size, indicator_size),
                            window.failed ? colors.danger : colors.accent, indicator_size / 2);
            });
        !ready) {
      return ready;
    }
    float scale = window.dpi / 96.0f;
    if (auto ready =
            composition::place(window.indicator.node, 20 * scale, indicator_top * scale,
                               D2D1::RectF(0, 0, indicator_size * scale, indicator_size * scale));
        !ready) {
      return ready;
    }
  }
  // 扫描进行中触发圆点正弦呼吸动效
  if (auto ready =
          composition::pulse(state, window.indicator.node, window.active && window.visible);
      !ready) {
    return ready;
  }
  // 非进行态装配查看与复制按钮
  std::vector<button::Spec> buttons;
  auto clip = D2D1::RectF(0, 0, width, height);
  float action_right = width - 20;
  float action_bottom = height - 18;
  float action_top = action_bottom - 32;
  if (!window.active) {
    if (window.success) {
      buttons.push_back({.action = button::Action::view,
                         .label = core::i18n::wide(state, "action.view"),
                         .bounds = D2D1::RectF(20, action_top, action_right - 112, action_bottom),
                         .clip = clip,
                         .style = button::Style::primary});
      buttons.push_back(
          {.action = button::Action::copy,
           .label = window.copied ? core::i18n::wide(state, "action.copied")
                                  : core::i18n::wide(state, "action.copy"),
           .bounds = D2D1::RectF(action_right - 100, action_top, action_right, action_bottom),
           .clip = clip,
           .style = button::Style::secondary});
    } else {
      buttons.push_back(
          {.action = button::Action::view,
           .label = core::i18n::wide(state, "action.view"),
           .bounds = D2D1::RectF(action_right - 120, action_top, action_right, action_bottom),
           .clip = clip,
           .style = button::Style::primary});
    }
  }
  // 同步按钮集合
  if (auto ready =
          button::sync(state, window.buttons, window.hwnd, window.composition.root.visual.Get(),
                       std::move(buttons), colors, window.dpi, window.repaint_buttons);
      !ready) {
    return ready;
  }
  // 提交合成渲染
  auto committed = composition::commit(state);
  if (committed) {
    window.redraw = false;
    window.repaint_buttons = false;
  }
  return committed;
}

}  // namespace ui::status_window::painter

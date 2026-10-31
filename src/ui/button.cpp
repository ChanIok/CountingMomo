#include "ui/button.hpp"

#include "vendor/std.hpp"

#include "core/message/message.hpp"
#include "ui/drawing.hpp"

namespace ui::button {
namespace {

// 判断两个矩形边界是否完全一致
auto same_rect(D2D1_RECT_F first, D2D1_RECT_F second) -> bool {
  return first.left == second.left && first.top == second.top && first.right == second.right &&
         first.bottom == second.bottom;
}

// 判断指定二维坐标是否落在矩形区域内
auto contains(D2D1_RECT_F rect, D2D1_POINT_2F point) -> bool {
  return point.x >= rect.left && point.x < rect.right && point.y >= rect.top &&
         point.y < rect.bottom;
}

// 测试光标落在哪个可用按钮上：按可见区域与裁剪盒命中测试
auto hit(const State& state, D2D1_POINT_2F point) -> Action {
  for (const auto& button : state.buttons) {
    if (button.spec.enabled && contains(button.spec.bounds, point) &&
        contains(button.spec.clip, point)) {
      return button.spec.action;
    }
  }
  return Action::none;
}

// 释放按钮按下手势：清空按下状态并安全释放鼠标独占捕获
auto release_press(State& state, HWND hwnd) -> void {
  auto pressed = std::exchange(state.pressed, Action::none);
  // 同一 HWND 也会被系统拖动和缩放捕获；只能释放按钮手势自己取得的捕获。
  // 先清空手势，避免 ReleaseCapture 同步发送 WM_CAPTURECHANGED 时再次取消。
  if (pressed != Action::none && GetCapture() == hwnd) {
    ReleaseCapture();
  }
}

// 刷新所有按钮的悬停、按下与文字淡入淡出动画过渡
auto animate(core::AppState& app, State& state) -> std::expected<void, core::Error> {
  bool high_contrast = theme::resolve(app).high_contrast;
  for (auto& button : state.buttons) {
    bool hovered = button.spec.enabled && state.hovered == button.spec.action;
    bool pressed = hovered && state.pressed == button.spec.action;
    // 悬停背景图层淡入淡出
    if (auto ready = composition::opacity(app, button.hover.node, hovered ? 1.0f : 0.0f,
                                          button.spec.enabled ? (hovered ? 0.12 : 0.16) : 0);
        !ready) {
      return ready;
    }
    // 按下背景图层淡入淡出
    if (auto ready = composition::opacity(app, button.pressed.node, pressed ? 1.0f : 0.0f,
                                          pressed || !button.spec.enabled ? 0 : 0.08);
        !ready) {
      return ready;
    }
    // 主要按钮与次要按钮按下时文字轻微收敛，形成下沉触感反馈
    bool soften_ink =
        pressed && (button.spec.style == Style::secondary || button.spec.style == Style::primary) &&
        !high_contrast;
    float ink_opacity = soften_ink ? (button.spec.style == Style::primary ? 0.80f : 0.85f) : 1.0f;
    if (auto ready =
            composition::opacity(app, button.ink.node, ink_opacity,
                                 soften_ink || !button.spec.enabled || high_contrast ? 0 : 0.08);
        !ready) {
      return ready;
    }
  }
  return {};
}

// 绘制按钮多层纹理：绘制常态/悬停/按下 3 张背景图层及前景文字/图标墨迹
auto paint(core::AppState& app, Button& button, const theme::Palette& colors, UINT dpi)
    -> std::expected<void, core::Error> {
  const auto& spec = button.spec;
  float width = spec.bounds.right - spec.bounds.left;
  float height = spec.bounds.bottom - spec.bounds.top;
  // 计算物理像素尺寸
  UINT pixel_width = static_cast<UINT>(std::max(1.0f, std::ceil(width * dpi / 96.0f)));
  UINT pixel_height = static_cast<UINT>(std::max(1.0f, std::ceil(height * dpi / 96.0f)));
  bool primary = spec.style == Style::primary;
  bool secondary = spec.style == Style::secondary;
  auto normal = primary ? colors.accent : secondary ? colors.secondary : colors.soft;
  auto foreground = primary ? colors.accent_text : secondary ? colors.secondary_text : colors.text;
  // 禁用态色彩降阶
  if (!spec.enabled) {
    normal = colors.high_contrast ? colors.soft : colors.line;
    foreground = colors.muted;
  }
  if (spec.style == Style::quiet) {
    normal.a = 0;
  }
  std::array<composition::Layer*, 3> backgrounds{&button.base, &button.hover, &button.pressed};
  std::array<D2D1_COLOR_F, 3> fills{normal,
                                    primary     ? colors.accent_hover
                                    : secondary ? colors.secondary_hover
                                                : colors.soft_hover,
                                    primary     ? colors.accent_pressed
                                    : secondary ? colors.secondary_pressed
                                                : colors.soft_pressed};
  auto background_bounds = secondary ? D2D1::RectF(0.5f, 0.5f, width - 0.5f, height - 0.5f)
                                     : D2D1::RectF(1, 1, width - 1, height - 1);
  float radius = primary ? 7.0f : 4.0f;
  // 分别离屏绘制常态、悬停态与按下态 3 种背景切片
  for (std::size_t index = 0; index < backgrounds.size(); ++index) {
    if (auto ready = composition::draw(app, *backgrounds[index], pixel_width, pixel_height, dpi,
                                       [&](auto* target, auto) {
                                         drawing::fill(target, background_bounds, fills[index],
                                                       radius);
                                       });
        !ready) {
      return ready;
    }
  }
  // 绘制最顶层墨迹图层：包含次要按钮边框描边、图标与文本排版
  return composition::draw(
      app, button.ink, pixel_width, pixel_height, dpi, [&](auto* target, auto) {
        if (secondary) {
          // 常驻轮廓与文字同层，悬停背景不能覆盖描边，也不叠加额外焦点外框。
          auto border =
              spec.enabled || colors.high_contrast ? colors.secondary_border : colors.line;
          drawing::outline(target, background_bounds, border, radius);
        }
        if (!spec.icon.empty()) {
          // 渲染 Segoe MDL2 Assets 字体图标及跟随文本
          drawing::text(app, target, spec.icon, D2D1::RectF(12, 0, 26, height), 12, foreground,
                        drawing::Alignment::center, L"Segoe MDL2 Assets");
          drawing::text(app, target, spec.label, D2D1::RectF(32, 0, width - 8, height), 13,
                        foreground);
        } else {
          // 单纯居中文本排版
          drawing::text(app, target, spec.label, D2D1::RectF(0, 0, width, height),
                        height <= 34 ? 13.0f : 14.0f, foreground, drawing::Alignment::center);
        }
      });
}

}  // namespace

// 同步按钮视觉树与规格描述：移除弃用节点 → 增量创建图层 → 按需重绘与排版定位 → 探测光标命中
auto sync(core::AppState& app, State& state, HWND hwnd, IDCompositionVisual2* parent,
          std::vector<Spec> specs, const theme::Palette& colors, UINT dpi, bool repaint)
    -> std::expected<void, core::Error> {
  // 移除不可见元素，而不是保留一组隐藏 HWND 或离屏命中区域。
  for (auto iterator = state.buttons.begin(); iterator != state.buttons.end();) {
    bool visible = std::ranges::any_of(
        specs, [&](const Spec& spec) { return spec.action == iterator->spec.action; });
    if (!visible) {
      if (auto result = parent->RemoveVisual(iterator->root.visual.Get()); FAILED(result)) {
        return std::unexpected(core::error("error.ui.remove_button_failed"));
      }
      iterator = state.buttons.erase(iterator);
    } else {
      ++iterator;
    }
  }
  // 遍历目标配置，增量创建或更新按钮实体
  for (auto& spec : specs) {
    auto found = std::ranges::find_if(
        state.buttons, [&](const Button& button) { return button.spec.action == spec.action; });
    if (found == state.buttons.end()) {
      state.buttons.emplace_back();
      found = std::prev(state.buttons.end());
      if (auto ready = composition::attach(app, found->root, parent); !ready) {
        return ready;
      }
      // attach 将新节点放在最前层，文字必须最后添加，始终位于背景动画之上。
      for (auto* layer : {&found->base, &found->hover, &found->pressed, &found->ink}) {
        if (auto ready = composition::attach(app, layer->node, found->root.visual.Get()); !ready) {
          return ready;
        }
      }
      if (auto ready = composition::opacity(app, found->hover.node, 0); !ready) {
        return ready;
      }
      if (auto ready = composition::opacity(app, found->pressed.node, 0); !ready) {
        return ready;
      }
    }
    auto& button = *found;
    // 检查外观、文案或尺寸是否发生变化需要重绘
    bool changed =
        repaint || button.dpi != dpi || button.spec.label != spec.label ||
        button.spec.style != spec.style || button.spec.enabled != spec.enabled ||
        button.spec.icon != spec.icon ||
        button.spec.bounds.right - button.spec.bounds.left !=
            spec.bounds.right - spec.bounds.left ||
        button.spec.bounds.bottom - button.spec.bounds.top != spec.bounds.bottom - spec.bounds.top;
    bool moved = !same_rect(button.spec.bounds, spec.bounds) ||
                 !same_rect(button.spec.clip, spec.clip) || button.dpi != dpi;
    if (state.pressed == spec.action &&
        (moved || button.spec.label != spec.label || button.spec.enabled != spec.enabled)) {
      // 一次按下不能跨越布局或按钮语义的改变，尤其是“停止”切换为“复制”。
      release_press(state, hwnd);
    }
    button.spec = std::move(spec);
    // 重绘按钮纹理
    if (changed) {
      if (auto ready = paint(app, button, colors, dpi); !ready) {
        return ready;
      }
    }
    // 布局移动或裁剪变化时更新合成坐标与裁剪矩形
    if (moved) {
      const auto& bounds = button.spec.bounds;
      const auto& clip = button.spec.clip;
      float scale = dpi / 96.0f;
      auto local_clip =
          D2D1::RectF(std::max(0.0f, clip.left - bounds.left) * scale,
                      std::max(0.0f, clip.top - bounds.top) * scale,
                      std::max(0.0f, std::min(bounds.right, clip.right) - bounds.left) * scale,
                      std::max(0.0f, std::min(bounds.bottom, clip.bottom) - bounds.top) * scale);
      // 完全位于视口外时使用空裁剪，不能把下边界设在上边界之前。
      local_clip.left = std::min(local_clip.left, local_clip.right);
      local_clip.top = std::min(local_clip.top, local_clip.bottom);
      if (auto ready =
              composition::place(button.root, bounds.left * scale, bounds.top * scale, local_clip);
          !ready) {
        return ready;
      }
    }
    button.dpi = dpi;
  }
  // 检查已按下的按钮是否已被禁用或移除
  auto available = [&](Action action) {
    return std::ranges::any_of(state.buttons, [&](const Button& button) {
      return button.spec.action == action && button.spec.enabled;
    });
  };
  if (!available(state.pressed)) {
    release_press(state, hwnd);
  }
  // 滚动、切页和 DPI 变化也会改变指针下的按钮，不依赖下一笔 WM_MOUSEMOVE。
  state.hovered = Action::none;
  POINT cursor{};
  if (GetCursorPos(&cursor) && WindowFromPoint(cursor) == hwnd && ScreenToClient(hwnd, &cursor)) {
    state.hovered = hit(state, D2D1::Point2F(cursor.x * 96.0f / dpi, cursor.y * 96.0f / dpi));
  }
  return animate(app, state);
}

// 处理窗口鼠标输入消息：驱动悬停、按下、拖拽判定及按钮点击动作触发
auto pointer(core::AppState& app, State& state, HWND hwnd, UINT message, LPARAM lparam, UINT dpi)
    -> std::expected<Action, core::Error> {
  auto previous_hovered = state.hovered;
  auto previous_pressed = state.pressed;
  float scale = 96.0f / dpi;
  auto point = D2D1::Point2F(static_cast<short>(LOWORD(lparam)) * scale,
                             static_cast<short>(HIWORD(lparam)) * scale);
  Action action = Action::none;
  // 鼠标离开窗口客户区
  if (message == WM_MOUSELEAVE) {
    state.hovered = Action::none;
  } else {
    // 命中测试探测光标所在按钮
    state.hovered = hit(state, point);
    if (message == WM_MOUSEMOVE) {
      // 鼠标移动且左键已松开时清理按压残留
      if (state.pressed != Action::none && !(GetKeyState(VK_LBUTTON) & 0x8000)) {
        release_press(state, hwnd);
      }
      TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
      TrackMouseEvent(&track);
    } else if (message == WM_LBUTTONDOWN && state.hovered != Action::none) {
      // 左键按下时捕获全局鼠标以跟踪拖拽滑出
      state.pressed = state.hovered;
      SetCapture(hwnd);
      if (GetCapture() != hwnd) {
        state.pressed = Action::none;
      }
    } else if (message == WM_LBUTTONUP) {
      // 在同一按钮上抬起时触发动作
      if (state.pressed == state.hovered) {
        action = state.pressed;
      }
      release_press(state, hwnd);
    }
  }
  // 交互状态发生迁移时触发平滑过渡动效
  if (state.hovered != previous_hovered || state.pressed != previous_pressed) {
    if (auto ready = animate(app, state); !ready) {
      return std::unexpected(ready.error());
    }
  }
  return action;
}

// 取消当前窗口的所有按钮交互状态：重置悬停与按下并平滑复位动画
auto cancel(core::AppState& app, State& state, HWND hwnd) -> std::expected<void, core::Error> {
  state.hovered = Action::none;
  release_press(state, hwnd);
  return animate(app, state);
}

}  // namespace ui::button

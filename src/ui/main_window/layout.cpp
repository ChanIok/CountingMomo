#include "ui/main_window/layout.hpp"

#include "vendor/std.hpp"

#include "core/i18n/i18n.hpp"
#include "core/state/app_state.hpp"
#include "ui/main_window/state.hpp"

namespace ui::main_window::layout {
namespace {
auto measure(const core::AppState& state, float width, float viewport) -> Layout {
  const auto& window = *state.main_window;
  const auto& view = window.view;
  Layout result;
  float footer_top = std::max(header_bottom + 16, viewport - 24 - footer_height);
  result.footer = D2D1::RectF(24, footer_top, width - 24, footer_top + footer_height);
  result.body_bottom = footer_top - 16;
  result.settings = D2D1::RectF(width - 132, 48, width - 24, 80);
  bool chinese = core::i18n::current(state) == core::i18n::Language::zh_cn;
  result.title = D2D1::RectF(24, 48, chinese ? 136 : result.settings.left - 16, 80);
  // 五个 20 DIP 全宽汉字占 100 DIP；标题区域保留余量，分隔点按文字末端定位。
  float title_text_right = result.title.left + 100;
  float separator_top = (result.title.top + result.title.bottom) / 2 - 1;
  result.title_separator =
      D2D1::RectF(title_text_right + 8, separator_top, title_text_right + 10, separator_top + 2);
  result.product_name =
      D2D1::RectF(result.title_separator.right + 8, 50, result.settings.left - 16, 82);

  // 临时提示覆盖绘制，不参与排版：出现和消失都不移动卡片，也不扩大滚动范围。
  float left = 48;
  float right = width - 48;
  float cursor = 128;
  auto row = [&](float height, float gap) {
    auto rect = D2D1::RectF(left, cursor, right, cursor + height);
    cursor += height + gap;
    return rect;
  };
  result.heading = row(28, 0);
  bool alternate = view.page != Page::main;
  float card_bottom = std::max(328.0f, result.body_bottom);
  if (view.page == Page::settings) {
    cursor += 20;
    result.theme = row(44, 10);
    result.language = row(44, 10);
    result.restore = row(44, 0);
    card_bottom = cursor + 24;
  } else if (view.page == Page::details) {
    cursor += 20;
    bool warning = !view.warning.empty();
    float list_height = view.summary_count * 56.0f;
    result.summary = row(list_height, warning ? 16.0f : 0.0f);
    if (warning) {
      result.description = row(44, 0);
    }
    card_bottom = cursor + 24;
  } else if (view.phase == app::Phase::completed && view.has_success) {
    result.heading.right = right - 156;
    result.elapsed = D2D1::RectF(right - 140, result.heading.top, right, result.heading.bottom);
    cursor += 12;
    result.metrics = row(68, 0);
    result.divider_y = card_bottom - 88;
  } else if ((view.phase == app::Phase::running || view.phase == app::Phase::stopping)) {
    cursor += 8;
    result.description = row(26, 8);
    result.metrics = row(56, 0);
  } else if (view.phase == app::Phase::ready) {
    cursor += 8;
    result.description = row(48, 0);
  } else {
    cursor += 16;
    result.description = row(56, 0);
  }
  result.card = D2D1::RectF(24, 104, width - 24, card_bottom);
  if (!alternate) {
    result.primary = D2D1::RectF(right - 144, card_bottom - 68, right, card_bottom - 24);
    result.action_description =
        D2D1::RectF(left, result.primary.top, result.primary.left - 20, result.primary.bottom);
  }

  // 提示条贴在卡片底部操作行的左半侧，右边界止于主按钮，长文案换行也不会压到按钮。
  // 设置和明细页没有底部主按钮，改为贴正文下沿，避免盖住页内按钮行。
  float notice_bottom = alternate ? result.body_bottom - 12 : card_bottom - 24;
  float notice_right = alternate ? width - 72 : result.primary.left - 16;
  result.notice = D2D1::RectF(left, notice_bottom - 48, notice_right, notice_bottom);

  // 所有页面复用这一条 32 DIP 底栏，状态变化不影响其高度与位置。
  auto footer_button = [&](float x, float button_width) {
    return D2D1::RectF(x, footer_top, x + button_width, footer_top + footer_height);
  };
  result.diagnostic = footer_button(20, 116);
  result.details = footer_button(20, 76);
  result.save = footer_button(104, 94);
  result.secondary = footer_button(width - 120, 100);
  if (view.page == Page::details) {
    result.save = footer_button(width - 106, 86);
  } else if (view.page != Page::settings && view.phase != app::Phase::completed &&
             view.has_success) {
    result.secondary = footer_button(width - 204, 184);
  }
  result.content_height = card_bottom;
  return result;
}

}  // namespace

auto update(core::AppState& state) -> void {
  auto& window = *state.main_window;
  float scale = 96.0f / window.dpi;
  window.layout = measure(state, window.width * scale, window.height * scale);
  window.scroll_y =
      std::clamp(window.scroll_y, 0.0f,
                 std::max(0.0f, window.layout.content_height - window.layout.body_bottom));
}

}  // namespace ui::main_window::layout

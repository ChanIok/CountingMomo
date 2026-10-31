#include "ui/main_window/painter.hpp"

#include "vendor/std.hpp"

#include "vendor/windows/dwrite.hpp"
#include "vendor/windows/wrl/client.hpp"

#include "core/message/message.hpp"

#include "core/i18n/i18n.hpp"
#include "core/state/app_state.hpp"
#include "ui/button.hpp"
#include "ui/drawing.hpp"
#include "ui/main_window/state.hpp"
#include "ui/shared_render_resources/state.hpp"
#include "ui/theme.hpp"
#include "utils/string.hpp"

namespace ui::main_window::painter {
namespace {
auto format_count(int value) -> std::string {
  auto result = std::to_string(value);
  for (int position = static_cast<int>(result.size()) - 3; position > 0; position -= 3) {
    result.insert(static_cast<size_t>(position), 1, ',');
  }
  return result;
}

auto metrics(core::AppState& state, ID2D1RenderTarget* target, D2D1_RECT_F rect,
             std::wstring_view first_label, std::wstring_view first_value,
             std::wstring_view second_label, std::wstring_view second_value,
             const theme::Palette& colors, float value_size) -> void {
  float middle = (rect.left + rect.right) / 2;
  drawing::text(state, target, first_label,
                D2D1::RectF(rect.left, rect.top, middle - 20, rect.top + 20), 13, colors.muted);
  drawing::text(state, target, first_value,
                D2D1::RectF(rect.left, rect.top + 24, middle - 20, rect.bottom), value_size,
                colors.text);
  drawing::text(state, target, second_label,
                D2D1::RectF(middle + 24, rect.top, rect.right, rect.top + 20), 13, colors.muted);
  drawing::text(state, target, second_value,
                D2D1::RectF(middle + 24, rect.top + 24, rect.right, rect.bottom), value_size,
                colors.text);
}

auto buttons(core::AppState& state, const State& window) -> std::vector<button::Spec> {
  const auto& view = window.view;
  const auto& layout = window.layout;
  bool active = view.phase == app::Phase::running || view.phase == app::Phase::stopping;
  std::vector<button::Spec> result;
  auto add = [&](button::Action action, std::wstring label, D2D1_RECT_F bounds,
                 button::Style style = button::Style::quiet, bool enabled = true, bool body = false,
                 std::wstring icon = {}) {
    auto clip =
        D2D1::RectF(0, 0, window.width * 96.0f / window.dpi, window.height * 96.0f / window.dpi);
    if (body) {
      bounds.top -= window.scroll_y;
      bounds.bottom -= window.scroll_y;
      clip.top = ui::main_window::layout::header_bottom;
      clip.bottom = layout.body_bottom;
    }
    result.push_back({.action = action,
                      .label = std::move(label),
                      .bounds = bounds,
                      .clip = clip,
                      .style = style,
                      .enabled = enabled,
                      .icon = std::move(icon)});
  };
  if (view.page == Page::main) {
    add(button::Action::settings, core::i18n::wide(state, "action.settings"), layout.settings,
        button::Style::quiet, !active, false, L"\uE713");
    std::wstring label =
        view.phase == app::Phase::stopping ? core::i18n::wide(state, "action.stopping")
        : active                           ? core::i18n::wide(state, "action.stop")
        : view.phase == app::Phase::completed
            ? (view.copied ? core::i18n::wide(state, "action.copied")
                           : core::i18n::wide(state, "action.copy"))
        : view.phase == app::Phase::ready ? core::i18n::wide(state, "action.start")
                                          : core::i18n::wide(state, "action.retry");
    add(button::Action::primary, std::move(label), layout.primary, button::Style::primary,
        view.phase != app::Phase::stopping, true);
    if (!active && view.has_success) {
      add(button::Action::secondary,
          view.phase == app::Phase::completed ? core::i18n::wide(state, "action.scan_again")
          : view.copied                       ? core::i18n::wide(state, "action.previous_copied")
                                              : core::i18n::wide(state, "action.copy_previous"),
          layout.secondary);
    }
    if (view.phase == app::Phase::completed) {
      add(button::Action::details, core::i18n::wide(state, "action.details"), layout.details);
      add(button::Action::save, core::i18n::wide(state, "action.save"), layout.save);
    }
  } else {
    add(button::Action::back, core::i18n::wide(state, "action.back"), layout.settings,
        button::Style::quiet, true, false, L"\uE72B");
    if (view.page == Page::settings) {
      add(button::Action::theme, view.theme_label, layout.theme, button::Style::secondary, true,
          true);
      add(button::Action::language, view.language_label, layout.language, button::Style::secondary,
          true, true);
      add(button::Action::restore, view.restore_label, layout.restore, button::Style::secondary,
          true, true);
    } else {
      add(button::Action::save, core::i18n::wide(state, "action.save"), layout.save);
    }
  }
  bool failed = view.phase == app::Phase::failed || view.phase == app::Phase::stopped;
  if (view.diagnostic && (view.page == Page::settings || (view.page == Page::main && failed))) {
    add(button::Action::diagnostic, core::i18n::wide(state, "action.diagnostics"),
        layout.diagnostic, button::Style::quiet, !active);
  }
  return result;
}

}  // namespace

auto paint(core::AppState& state) -> std::expected<void, core::Error> {
  auto& window = *state.main_window;
  if (auto ready = composition::create(state, window.composition, window.hwnd); !ready) {
    return ready;
  }
  auto colors = theme::resolve(state);
  const auto& view = window.view;
  const auto& layout = window.layout;
  float width = window.width * 96.0f / window.dpi;
  if (window.redraw) {
    auto painted = composition::draw(
        state, window.composition.content, window.width, window.height, window.dpi,
        [&](auto* target, auto origin) {
          target->Clear(window.material ? D2D1::ColorF(0, 0.0f) : colors.background);
          drawing::text(state, target, core::i18n::wide(state, "app.name"), layout.title, 20,
                        colors.text);
          if (core::i18n::current(state) == core::i18n::Language::zh_cn) {
            drawing::fill(target, layout.title_separator, colors.muted, 1);
            drawing::text(state, target, L"CountingMomo", layout.product_name, 13, colors.muted);
          }
          target->PushAxisAlignedClip(
              D2D1::RectF(0, ui::main_window::layout::header_bottom, width, layout.body_bottom),
              D2D1_ANTIALIAS_MODE_ALIASED);
          target->SetTransform(D2D1::Matrix3x2F::Translation(0, -window.scroll_y) * origin);
          drawing::fill(target, layout.card, colors.card, 8);
          if (layout.divider_y > 0) {
            drawing::line(target, {layout.card.left + 24, layout.divider_y},
                          {layout.card.right - 24, layout.divider_y}, colors.line);
          }
          if (view.page == Page::settings) {
            drawing::text(state, target, core::i18n::wide(state, "settings.heading"),
                          layout.heading, 16, colors.text);
          } else if (view.page == Page::details) {
            drawing::text(state, target, core::i18n::wide(state, "action.details"), layout.heading,
                          16, colors.text);
            if (state.app->success) {
              float top = layout.summary.top;
              for (const auto& summary : state.app->success->summaries) {
                drawing::text(state, target, core::i18n::wide(state, "category." + summary.id),
                              D2D1::RectF(layout.summary.left, top, layout.summary.right, top + 24),
                              14, colors.text);
                drawing::text(
                    state, target,
                    core::i18n::wide(state, "summary.category_counts", summary.owned,
                                     summary.count),
                    D2D1::RectF(layout.summary.left, top + 24, layout.summary.right, top + 46), 13,
                    colors.muted);
                top += 56;
              }
            }
            if (!view.warning.empty()) {
              drawing::text(state, target, view.warning, layout.description, 13, colors.danger);
            }
          } else if (view.phase == app::Phase::completed && view.has_success) {
            drawing::text(state, target, core::i18n::wide(state, "scan.completed"), layout.heading,
                          16, colors.text);
            drawing::text(state, target,
                          core::i18n::wide(state, "summary.elapsed",
                                           std::format("{:.1f}", view.completed_seconds)),
                          layout.elapsed, 13, colors.muted);
            metrics(state, target, layout.metrics, core::i18n::wide(state, "summary.owned"),
                    utils::string::wide(format_count(view.owned)),
                    core::i18n::wide(state, "summary.unowned"),
                    utils::string::wide(format_count(view.total - view.owned)), colors, 30);
            drawing::text(state, target,
                          core::i18n::wide(state, "summary.total", format_count(view.total)),
                          layout.action_description, 13, colors.muted);
          } else if ((view.phase == app::Phase::running || view.phase == app::Phase::stopping)) {
            drawing::text(state, target,
                          view.phase == app::Phase::stopping
                              ? core::i18n::wide(state, "scan.stopping")
                              : core::i18n::wide(state, "scan.running"),
                          layout.heading, 16, colors.text);
            auto category = view.phase == app::Phase::stopping
                                ? core::i18n::wide(state, "scan.finishing")
                            : view.category.empty() ? core::i18n::wide(state, "scan.preparing")
                                                    : view.category;
            drawing::text(state, target, category, layout.description, 14, colors.muted);
            // 分类大小未知，不把分类个数换算为耗时百分比。
            auto elapsed = view.elapsed;
            metrics(state, target, layout.metrics, core::i18n::wide(state, "scan.items_read"),
                    core::i18n::wide(state, "scan.item_count", view.category_count),
                    core::i18n::wide(state, "scan.elapsed"),
                    std::format(L"{:02}:{:02}", elapsed / 60, elapsed % 60), colors, 22);
            if (view.phase == app::Phase::running) {
              drawing::text(state, target, core::i18n::wide(state, "scan.stop_hint"),
                            layout.action_description, 13, colors.muted);
            }
          } else if (view.phase == app::Phase::ready) {
            drawing::text(state, target, core::i18n::wide(state, "scan.heading"), layout.heading,
                          16, colors.text);
            drawing::text(state, target, core::i18n::wide(state, "scan.instructions"),
                          layout.description, 14, colors.muted);
            bool found = view.game_found;
            drawing::text(state, target,
                          found ? core::i18n::wide(state, "scan.game_running")
                                : core::i18n::wide(state, "scan.start_game"),
                          layout.action_description, 13, colors.muted);
          } else {
            drawing::text(state, target,
                          view.phase == app::Phase::stopped
                              ? core::i18n::wide(state, "scan.stopped")
                              : core::i18n::wide(state, "scan.failed"),
                          layout.heading, 16, colors.text);
            drawing::text(state, target, view.message, layout.description, 14, colors.danger);
          }
          target->SetTransform(origin);
          if (!view.notice.empty()) {
            // 提示是覆盖条：不随正文滚动，也不改变卡片位置和滚动范围。
            drawing::fill(target, layout.notice, colors.soft, 8);
            drawing::outline(target, layout.notice, colors.line, 8, 1);
            drawing::text(state, target, view.notice,
                          D2D1::RectF(layout.notice.left + 12, layout.notice.top + 4,
                                      layout.notice.right - 12, layout.notice.bottom - 4),
                          13, colors.text);
          }
          target->PopAxisAlignedClip();
          if (view.page == Page::main && view.phase == app::Phase::ready) {
            drawing::text(state, target, core::i18n::wide(state, "scan.footer"), layout.footer, 13,
                          colors.muted);
          }
        });
    if (!painted) {
      return painted;
    }
  }
  if (auto ready =
          button::sync(state, window.buttons, window.hwnd, window.composition.root.visual.Get(),
                       buttons(state, window), colors, window.dpi, window.repaint_buttons);
      !ready) {
    return ready;
  }
  auto committed = composition::commit(state);
  if (committed) {
    window.redraw = false;
    window.repaint_buttons = false;
  }
  return committed;
}

}  // namespace ui::main_window::painter

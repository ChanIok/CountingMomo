#include "features/scan/layout.hpp"

#include "vendor/std.hpp"

namespace features::scan {

namespace {

constexpr Layout layout{
    .reference_size = {1440, 5760},
    .stability_rect = {180, 0, 1440, 5760},
    .columns = {304, 669, 1033},
    .columns_without_scrollbar = {304, 672, 1040},
    .first_row_y = 157,
    .row_step = 132,
    .card_size = {325, 110},
    .safe_y = {150, 5420},
    .scrolled_safe_top = 540,
    .bottom_safe_y = 5685,
    .scroll_rect = {304, 300, 1398, 5420},
    .name_tracking_rect = {135, 6, 285, 36},
};

static_assert(layout.row_step >= layout.card_size[1]);
static_assert(0 <= layout.safe_y[0] && layout.safe_y[0] <= layout.first_row_y &&
              layout.first_row_y + layout.card_size[1] <= layout.scrolled_safe_top &&
              layout.scrolled_safe_top < layout.safe_y[1] &&
              layout.safe_y[1] <= layout.bottom_safe_y &&
              layout.bottom_safe_y <= layout.reference_size[1]);
static_assert([] {
  for (const auto& columns : {layout.columns, layout.columns_without_scrollbar}) {
    int last = -layout.card_size[0];
    for (int x : columns) {
      if (x < last + layout.card_size[0] || x + layout.card_size[0] > layout.reference_size[0]) {
        return false;
      }
      last = x;
    }
  }
  for (auto box : {layout.stability_rect, layout.scroll_rect}) {
    if (box[0] < 0 || box[1] < 0 || box[0] >= box[2] || box[1] >= box[3] ||
        box[2] > layout.reference_size[0] || box[3] > layout.reference_size[1]) {
      return false;
    }
  }
  const auto& name = layout.name_tracking_rect;
  return name[0] >= 0 && name[1] >= 0 && name[0] < name[2] && name[1] < name[3] &&
         name[2] <= layout.card_size[0] && name[3] <= layout.card_size[1];
}());

}  // namespace

auto get_layout() -> const Layout& { return layout; }

auto rect(const Box& box) -> cv::Rect { return {box[0], box[1], box[2] - box[0], box[3] - box[1]}; }

}  // namespace features::scan

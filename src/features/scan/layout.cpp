#include "features/scan/layout.hpp"

#include "vendor/std.hpp"

namespace features::scan {

namespace {

constexpr Layout layout{
    .reference_size = {960, 3840},
    .stability_rect = {120, 0, 960, 3840},
    .columns = {203, 446, 689},
    .columns_without_scrollbar = {203, 448, 693},
    .first_row_y = 105,
    .row_step = 88,
    .card_size = {217, 73},
    .safe_y = {100, 3613},
    .scrolled_safe_top = 360,
    .bottom_safe_y = 3790,
    .scroll_rect = {203, 200, 932, 3613},
    .name_tracking_rect = {90, 4, 190, 24},
};

}  // namespace

auto get_layout() -> const Layout& { return layout; }

auto rect(const Box& box) -> cv::Rect { return {box[0], box[1], box[2] - box[0], box[3] - box[1]}; }

}  // namespace features::scan

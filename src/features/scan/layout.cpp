#include "features/scan/layout.hpp"

#include "vendor/std.hpp"

#include "vendor/rfl.hpp"

#include "core/message/message.hpp"

#include "utils/file.hpp"

namespace features::scan {

namespace {

auto box_within(const Box& box, int width, int height) -> bool {
  return box[0] >= 0 && box[1] >= 0 && box[2] > box[0] && box[3] > box[1] && box[2] <= width &&
         box[3] <= height;
}

auto validate_layout(const Layout& layout) -> std::expected<void, core::Error> {
  const int image_width = layout.reference_size[0];
  const int image_height = layout.reference_size[1];
  const int card_width = layout.card_size[0];
  const int card_height = layout.card_size[1];
  if (image_width <= 0 || image_height <= 0 || image_width > 16384 || image_height > 16384 ||
      card_width <= 0 || card_height <= 0 || layout.row_step < card_height ||
      layout.columns.empty()) {
    return std::unexpected(core::error("error.layout.invalid_grid"));
  }
  if (layout.first_row_y < 0 || layout.first_row_y + card_height > image_height ||
      layout.safe_y[0] < 0 || layout.safe_y[0] >= layout.scrolled_safe_top ||
      layout.scrolled_safe_top >= layout.safe_y[1] || layout.safe_y[1] > image_height ||
      layout.bottom_detection.safe_bottom < layout.safe_y[1] ||
      layout.bottom_detection.safe_bottom > image_height) {
    return std::unexpected(core::error("error.layout.invalid_safe_bounds"));
  }
  if (layout.columns_without_scrollbar.size() != layout.columns.size()) {
    return std::unexpected(core::error("error.layout.column_count_mismatch"));
  }
  for (const auto* columns : {&layout.columns, &layout.columns_without_scrollbar}) {
    int last = -card_width;
    for (int x : *columns) {
      if (x < 0 || x < last + card_width || x + card_width > image_width) {
        return std::unexpected(core::error("error.layout.invalid_columns"));
      }
      last = x;
    }
  }
  // 卡片采样区域相对卡片左上角，全局区域相对客户区左上角。
  for (auto box : {layout.name_tracking_rect, layout.star_rect, layout.hidden_card.search_rect,
                   layout.card_frame.bottom_rect, layout.card_frame.right_rect}) {
    if (!box_within(box, card_width, card_height)) {
      return std::unexpected(core::error("error.layout.card_sample_outside"));
    }
  }
  if (layout.hidden_card.image.empty() || !std::isfinite(layout.hidden_card.min_score) ||
      layout.hidden_card.min_score <= 0 || layout.hidden_card.min_score > 1) {
    return std::unexpected(core::error("error.layout.invalid_question_template"));
  }
  if (layout.card_frame.bottom_rect[3] - layout.card_frame.bottom_rect[1] < 5 ||
      layout.card_frame.right_rect[2] - layout.card_frame.right_rect[0] < 5) {
    return std::unexpected(core::error("error.layout.border_band_too_small"));
  }
  const auto& bottom = layout.bottom_detection;
  for (auto box : {bottom.line_rect, bottom.left_rect, bottom.right_rect, layout.scroll_rect}) {
    if (!box_within(box, image_width, image_height)) {
      return std::unexpected(core::error("error.layout.sample_outside_image"));
    }
  }
  if (bottom.line_rect[1] < layout.safe_y[0] ||
      bottom.line_rect[3] - bottom.line_rect[1] != bottom.left_rect[3] - bottom.left_rect[1] ||
      bottom.line_rect[3] - bottom.line_rect[1] != bottom.right_rect[3] - bottom.right_rect[1] ||
      !std::isfinite(bottom.min_contrast) || bottom.min_contrast <= 0 ||
      !std::isfinite(bottom.min_clear_fraction) || bottom.min_clear_fraction <= 0 ||
      bottom.min_clear_fraction > 1 || !std::isfinite(layout.card_frame.min_contrast) ||
      layout.card_frame.min_contrast <= 0) {
    return std::unexpected(core::error("error.layout.invalid_border_threshold"));
  }
  // 拥有与未拥有的亮度区间必须分离，掩码保持原生尺寸。
  const auto& ownership = layout.ownership_star;
  if (!std::isfinite(ownership.owned_luma) || !std::isfinite(ownership.unowned_luma) ||
      !std::isfinite(ownership.max_luma_error) || ownership.max_luma_error <= 0 ||
      ownership.owned_luma - ownership.unowned_luma <= 2 * ownership.max_luma_error) {
    return std::unexpected(core::error("error.layout.invalid_star_luma"));
  }
  if (ownership.foreground_mask.empty() || ownership.foreground_mask[0].empty()) {
    return std::unexpected(core::error("error.layout.empty_star_mask"));
  }
  if (ownership.foreground_mask.size() !=
          static_cast<std::size_t>(layout.star_rect[3] - layout.star_rect[1]) ||
      ownership.foreground_mask[0].size() !=
          static_cast<std::size_t>(layout.star_rect[2] - layout.star_rect[0])) {
    return std::unexpected(core::error("error.layout.star_mask_size_mismatch"));
  }
  std::size_t pixels = 0;
  for (const auto& row : ownership.foreground_mask) {
    if (row.size() != ownership.foreground_mask[0].size() ||
        row.find_first_not_of("01") != std::string::npos) {
      return std::unexpected(core::error("error.layout.invalid_star_mask"));
    }
    pixels += std::count(row.begin(), row.end(), '1');
  }
  if (!pixels) {
    return std::unexpected(core::error("error.layout.no_star_foreground"));
  }
  return {};
}

}  // namespace

auto rect(const Box& box) -> cv::Rect { return {box[0], box[1], box[2] - box[0], box[3] - box[1]}; }

auto load_layout(const std::filesystem::path& path) -> std::expected<Layout, core::Error> {
  auto data = utils::file::read(path);
  if (!data) {
    return std::unexpected(data.error());
  }
  auto parsed = rfl::json::read<Layout>(*data);
  if (!parsed) {
    return std::unexpected(
        core::with_detail(core::error("error.layout.parse_failed"), parsed.error().what()));
  }
  Layout layout = std::move(parsed.value());
  if (auto validated = validate_layout(layout); !validated) {
    return std::unexpected(validated.error());
  }
  return layout;
}

}  // namespace features::scan

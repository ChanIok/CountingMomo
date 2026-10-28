#include "features/scan/types.hpp"

#include "vendor/std.hpp"

#include "vendor/rfl.hpp"

#include "utils/file.hpp"

namespace features::scan {
auto rect(const Box& box) -> cv::Rect { return {box[0], box[1], box[2] - box[0], box[3] - box[1]}; }
auto load_layout(const std::filesystem::path& path) -> std::expected<Layout, std::string> {
  auto data = utils::file::read(path);
  if (!data) return std::unexpected(data.error());
  auto parsed = rfl::json::read<Layout>(*data);
  if (!parsed) return std::unexpected("布局解析失败：" + parsed.error().what());
  Layout layout = std::move(parsed.value());
  int width = layout.reference_size[0], height = layout.reference_size[1];
  int cw = layout.card_size[0], ch = layout.card_size[1];
  auto within = [](const Box& b, int w, int h) {
    return b[0] >= 0 && b[1] >= 0 && b[2] > b[0] && b[3] > b[1] && b[2] <= w && b[3] <= h;
  };
  if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || cw <= 0 || ch <= 0 ||
      layout.row_step < ch || layout.columns.empty())
    return std::unexpected("布局尺寸或网格无效。");
  if (layout.first_row_y < 0 || layout.first_row_y + ch > height || layout.safe_y[0] < 0 ||
      layout.safe_y[0] >= layout.scrolled_safe_top ||
      layout.scrolled_safe_top >= layout.safe_y[1] || layout.safe_y[1] > height ||
      layout.bottom_detection.safe_bottom < layout.safe_y[1] ||
      layout.bottom_detection.safe_bottom > height)
    return std::unexpected("布局安全边界无效。");
  if (layout.columns_without_scrollbar.size() != layout.columns.size())
    return std::unexpected("有无滚动条的布局列数必须一致。");
  for (const auto* columns : {&layout.columns, &layout.columns_without_scrollbar}) {
    int last = -cw;
    for (int x : *columns) {
      if (x < 0 || x < last + cw || x + cw > width)
        return std::unexpected("卡片列超出画面或相互重叠。");
      last = x;
    }
  }
  for (auto b : {layout.name_rect, layout.name_tracking_rect, layout.star_rect,
                 layout.card_frame.bottom_rect, layout.card_frame.right_rect})
    if (!within(b, cw, ch)) return std::unexpected("卡片采样框超出卡片。");
  const auto& name = layout.name_recognition;
  int name_height = layout.name_rect[3] - layout.name_rect[1];
  int name_width = layout.name_rect[2] - layout.name_rect[0];
  if (name_width * name_height < 5 || name.font_size_range[0] < 1 ||
      name.font_size_range[0] > name.font_size_range[1] || name.font_size_range[1] >= name_height ||
      name_width < name.font_size_range[1] + 1 || name.baseline < name.font_size_range[1] ||
      name.baseline >= name_height || name.origin_count < 2 || name.origin_count > name_width)
    return std::unexpected("名称字号、基线或起点范围与裁图不符。");
  if (layout.card_frame.bottom_rect[3] - layout.card_frame.bottom_rect[1] < 5 ||
      layout.card_frame.right_rect[2] - layout.card_frame.right_rect[0] < 5)
    return std::unexpected("边框采样带至少5px。");
  auto& b = layout.bottom_detection;
  for (auto box : {b.line_rect, b.left_rect, b.right_rect, layout.scroll_rect})
    if (!within(box, width, height)) return std::unexpected("全局采样框超出画面。");
  if (b.line_rect[1] < layout.safe_y[0] ||
      b.line_rect[3] - b.line_rect[1] != b.left_rect[3] - b.left_rect[1] ||
      b.line_rect[3] - b.line_rect[1] != b.right_rect[3] - b.right_rect[1] ||
      !std::isfinite(b.min_contrast) || b.min_contrast <= 0 ||
      !std::isfinite(b.min_clear_fraction) || b.min_clear_fraction <= 0 ||
      b.min_clear_fraction > 1 || !std::isfinite(layout.card_frame.min_contrast) ||
      layout.card_frame.min_contrast <= 0)
    return std::unexpected("边框或滚动条阈值无效。");
  auto& s = layout.ownership_star;
  if (!std::isfinite(s.owned_luma) || !std::isfinite(s.unowned_luma) ||
      !std::isfinite(s.max_luma_error) || s.max_luma_error <= 0 ||
      s.owned_luma - s.unowned_luma <= 2 * s.max_luma_error)
    return std::unexpected("星星亮度范围无效或重叠。");
  if (s.foreground_mask.empty() || s.foreground_mask[0].empty())
    return std::unexpected("星星掩码为空。");
  if (s.foreground_mask.size() !=
          static_cast<std::size_t>(layout.star_rect[3] - layout.star_rect[1]) ||
      s.foreground_mask[0].size() !=
          static_cast<std::size_t>(layout.star_rect[2] - layout.star_rect[0]))
    return std::unexpected("星星掩码必须与原生像素裁图同尺寸，不缩放采样。");
  std::size_t pixels = 0;
  for (const auto& row : s.foreground_mask) {
    if (row.size() != s.foreground_mask[0].size() ||
        row.find_first_not_of("01") != std::string::npos)
      return std::unexpected("星星掩码无效。");
    pixels += std::count(row.begin(), row.end(), '1');
  }
  if (!pixels) return std::unexpected("星星掩码无前景。");
  return layout;
}
}  // namespace features::scan

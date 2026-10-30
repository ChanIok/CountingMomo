#include "features/scan/vision.hpp"

#include "vendor/std.hpp"

namespace features::scan {
namespace {
auto quantile(std::vector<float> values, double fraction) -> double {
  if (values.empty()) return 0;
  std::ranges::sort(values);
  double p = fraction * (values.size() - 1);
  auto a = static_cast<std::size_t>(p), b = std::min(a + 1, values.size() - 1);
  return values[a] + (values[b] - values[a]) * (p - a);
}
auto shifted(Box box, int x, int y) -> Box {
  return {box[0] + x, box[1] + y, box[2] + x, box[3] + y};
}
auto frame_contrast(const cv::Mat& gray, int x, int y, Box box, bool horizontal) -> double {
  auto roi = gray(rect(shifted(box, x, y)));
  std::vector<float> profile;
  int length = horizontal ? roi.rows : roi.cols;
  for (int index = 0; index < length; ++index) {
    auto line = horizontal ? roi.row(index) : roi.col(index);
    std::vector<float> pixels;
    for (int r = 0; r < line.rows; ++r)
      for (int c = 0; c < line.cols; ++c) pixels.push_back(line.at<std::uint8_t>(r, c));
    profile.push_back(static_cast<float>(quantile(std::move(pixels), 0.5)));
  }
  double a = (profile[0] + profile[1]) * 0.5, b = (profile[length - 2] + profile[length - 1]) * 0.5;
  return *std::max_element(profile.begin() + 2, profile.end() - 2) - std::min(a, b);
}
auto star_pixels(const cv::Mat& star, const OwnershipSettings& settings) -> cv::Mat {
  cv::Mat pixels;
  for (int y = 0; y < star.rows; ++y)
    for (int x = 0; x < star.cols; ++x) {
      if (settings.foreground_mask[y][x] != '1') continue;
      auto color = star.at<cv::Vec3b>(y, x);
      cv::Mat pixel(1, 1, CV_32FC3, cv::Scalar(color[0], color[1], color[2]));
      pixels.push_back(pixel);
    }
  return pixels;
}
auto scrollbar_visible(const cv::Mat& image, const Layout& layout) -> bool {
  const auto& settings = layout.bottom_detection;
  int top = layout.safe_y[0], bottom = settings.line_rect[3];
  int window = settings.line_rect[3] - settings.line_rect[1];
  std::array<Box, 3> boxes{settings.line_rect, settings.left_rect, settings.right_rect};
  std::array<cv::Mat, 3> bands;
  for (int i = 0; i < 3; ++i)
    bands[i] = image(cv::Rect(boxes[i][0], top, boxes[i][2] - boxes[i][0], bottom - top));
  std::vector<int> clear_rows;
  int clear = 0;
  for (int y = 0; y < bottom - top; ++y) {
    std::array<double, 3> means;
    for (int i = 0; i < 3; ++i) {
      auto mean = cv::mean(bands[i].row(y));
      means[i] = (mean[0] + mean[1] + mean[2]) / 3;
    }
    double contrast = means[0] - (means[1] + means[2]) / 2;
    clear_rows.push_back(contrast >= settings.min_contrast ? 1 : 0);
    clear += clear_rows.back();
    if (y >= window) clear -= clear_rows[y - window];
    if (y + 1 >= window && static_cast<double>(clear) / window >= settings.min_clear_fraction)
      return true;
  }
  return false;
}

auto difference_ok(const cv::Mat& first, const cv::Mat& second, double mean_limit) -> bool {
  if (first.size() != second.size() || first.type() != second.type()) return false;
  cv::Mat a, b, difference;
  first.convertTo(a, CV_32F);
  second.convertTo(b, CV_32F);
  cv::absdiff(a, b, difference);
  auto flat = difference.reshape(1, 1);
  if (cv::mean(flat)[0] > mean_limit) return false;
  std::vector<float> values(flat.ptr<float>(), flat.ptr<float>() + flat.total());
  return quantile(std::move(values), 0.99) <= 8;
}
}  // namespace
auto items_at_top(const cv::Mat& image, const Layout& layout) -> bool {
  if (!scrollbar_visible(image, layout)) return true;
  auto settings = layout.bottom_detection;
  int height = settings.line_rect[3] - settings.line_rect[1];
  for (auto* box : {&settings.line_rect, &settings.left_rect, &settings.right_rect}) {
    (*box)[1] = layout.safe_y[0];
    (*box)[3] = layout.safe_y[0] + height;
  }
  auto top_layout = layout;
  top_layout.bottom_detection = settings;
  return detect_bottom(image, top_layout).at_bottom;
}

auto detect_bottom(const cv::Mat& image, const Layout& layout) -> BottomResult {
  const auto& settings = layout.bottom_detection;
  std::array<std::vector<double>, 3> regions;
  std::array<Box, 3> boxes{settings.line_rect, settings.left_rect, settings.right_rect};
  for (int i = 0; i < 3; ++i) {
    auto roi = image(rect(boxes[i]));
    for (int y = 0; y < roi.rows; ++y) {
      auto mean = cv::mean(roi.row(y));
      regions[i].push_back((mean[0] + mean[1] + mean[2]) / 3);
    }
  }
  std::vector<float> contrasts;
  int clear = 0;
  for (std::size_t y = 0; y < regions[0].size(); ++y) {
    double contrast = regions[0][y] - (regions[1][y] + regions[2][y]) / 2;
    contrasts.push_back(static_cast<float>(contrast));
    if (contrast >= settings.min_contrast) ++clear;
  }
  double fraction = static_cast<double>(clear) / contrasts.size();
  return {fraction >= settings.min_clear_fraction, fraction, quantile(std::move(contrasts), 0.5)};
}
auto read_page(const cv::Mat& image, const Layout& layout, int phase, bool at_top,
               BottomResult bottom) -> std::expected<Page, std::string> {
  if (image.cols != layout.reference_size[0] || image.rows != layout.reference_size[1] ||
      image.type() != CV_8UC3)
    return std::unexpected("捕获尺寸与布局不符。");
  Page page;
  page.image = image;
  page.bottom = bottom;
  // 首屏没有滚动条时，按整个分类已在一屏内展示处理。
  page.single_page = at_top && !bottom.at_bottom && !scrollbar_visible(image, layout);
  const auto& columns = page.single_page ? layout.columns_without_scrollbar : layout.columns;
  cv::cvtColor(image, page.gray, cv::COLOR_BGR2GRAY);
  int top = at_top ? layout.safe_y[0] : layout.scrolled_safe_top;
  int limit = bottom.at_bottom ? layout.bottom_detection.safe_bottom : layout.safe_y[1];
  int step = layout.row_step, height = layout.card_size[1], width = layout.card_size[0];
  phase = (phase % step + step) % step;
  page.phase = phase;
  int first = phase + static_cast<int>(std::ceil(static_cast<double>(top - phase) / step)) * step;
  bool empty_tail = false;
  for (int y = first; y + height <= limit; y += step) {
    Row row{y, {}};
    for (int column = 0; column < static_cast<int>(columns.size()); ++column) {
      int x = columns[column];
      bool bottom_edge = frame_contrast(page.gray, x, y, layout.card_frame.bottom_rect, true) >=
                         layout.card_frame.min_contrast;
      bool right_edge = frame_contrast(page.gray, x, y, layout.card_frame.right_rect, false) >=
                        layout.card_frame.min_contrast;
      if (bottom_edge != right_edge)
        return std::unexpected(std::format("卡片边框尚未就绪（y={}，第{}列）。", y, column + 1));
      if (!bottom_edge) {
        if (!bottom.at_bottom && !page.single_page)
          return std::unexpected(std::format("卡片边框尚未出现（y={}，第{}列）。", y, column + 1));
        empty_tail = true;
        continue;
      }
      if (empty_tail) return std::unexpected("网格中间出现空位，等待刷新。");
      auto name_pixels = page.gray(rect(shifted(layout.name_tracking_rect, x, y)));
      cv::Scalar mean, deviation;
      cv::meanStdDev(name_pixels, mean, deviation);
      if (deviation[0] < 3)
        return std::unexpected(std::format("名称尚未就绪（y={}，第{}列）。", y, column + 1));
      Sample sample;
      sample.item.column = column + 1;
      sample.item.box = {x, y, x + width, y + height};
      sample.item.name_box = shifted(layout.name_rect, x, y);
      sample.item.star_box = shifted(layout.star_rect, x, y);
      sample.name = image(rect(sample.item.name_box));
      sample.star = image(rect(sample.item.star_box));
      page.names.push_back(name_pixels);
      page.stars.push_back(star_pixels(sample.star, layout.ownership_star));
      row.cards.push_back(std::move(sample));
    }
    if (!row.cards.empty()) page.rows.push_back(std::move(row));
  }
  if (page.rows.empty()) return std::unexpected("没有完整可读卡片行。");
  return page;
}
auto make_anchor(const Page& page, const Layout& layout, int overlap_rows)
    -> std::expected<Anchor, std::string> {
  if (overlap_rows < 2 || static_cast<int>(page.rows.size()) < overlap_rows)
    return std::unexpected("完整行不足以选择名称锚点。");
  Anchor anchor;
  int start = static_cast<int>(page.rows.size()) - overlap_rows;
  anchor.y = page.rows[start].y;
  for (int i = start; i < start + std::min(3, overlap_rows); ++i) {
    std::vector<cv::Mat> row;
    if (page.rows[i].cards.size() != layout.columns.size())
      return std::unexpected("锚点行存在空位。");
    for (int x : layout.columns)
      row.push_back(page.gray(rect(shifted(layout.name_tracking_rect, x, page.rows[i].y))).clone());
    anchor.templates.push_back(std::move(row));
  }
  return anchor;
}
auto locate_anchor(const cv::Mat& image, const Anchor& anchor, const Layout& layout, bool at_bottom)
    -> AnchorMatch {
  cv::Mat gray;
  cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
  int top = layout.scrolled_safe_top;
  int bottom = at_bottom ? layout.bottom_detection.safe_bottom : layout.safe_y[1];
  int last = bottom - layout.card_size[1] -
             (static_cast<int>(anchor.templates.size()) - 1) * layout.row_step;
  if (last < top) return {};
  cv::Mat raw_scores;
  for (int row = 0; row < static_cast<int>(anchor.templates.size()); ++row) {
    for (int col = 0; col < static_cast<int>(layout.columns.size()); ++col) {
      const auto& name = anchor.templates[row][col];
      cv::Scalar mean, deviation;
      cv::meanStdDev(name, mean, deviation);
      if (deviation[0] < 3) return {};
      int left = layout.columns[col] + layout.name_tracking_rect[0];
      int offset = layout.name_tracking_rect[1] + row * layout.row_step;
      cv::Mat response;
      cv::matchTemplate(gray(cv::Rect(left, 0, name.cols, gray.rows)), name, response,
                        cv::TM_CCOEFF_NORMED);
      cv::Mat row_scores = response.rowRange(top + offset, last + offset + 1).t();
      raw_scores.push_back(row_scores);
    }
  }
  // 整体位置始终取严格对齐的联合峰值，局部容差不改变位移或页面行相位。
  cv::Mat raw_average;
  cv::reduce(raw_scores, raw_average, 0, cv::REDUCE_AVG, CV_32F);
  cv::Point best;
  double raw_score{};
  cv::minMaxLoc(raw_average, nullptr, &raw_score, nullptr, &best);
  double raw_weakest{};
  cv::minMaxLoc(raw_scores.col(best.x), &raw_weakest, nullptr);

  // 每张名称仅在共同位置附近校正±1px；复用响应，不重做模板匹配或越过搜索边界。
  cv::Mat scores = raw_scores.clone();
  for (int row = 0; row < scores.rows; ++row) {
    const float* raw = raw_scores.ptr<float>(row);
    float* aligned = scores.ptr<float>(row);
    for (int i = 0; i < scores.cols; ++i) {
      if (i > 0) aligned[i] = std::max(aligned[i], raw[i - 1]);
      if (i + 1 < scores.cols) aligned[i] = std::max(aligned[i], raw[i + 1]);
    }
  }
  cv::Mat average;
  cv::reduce(scores, average, 0, cv::REDUCE_AVG, CV_32F);
  double score = average.at<float>(0, best.x);
  double weakest{};
  cv::minMaxLoc(scores.col(best.x), &weakest, nullptr);
  double runner = -1;
  for (int i = 0; i < average.cols; ++i)
    if (std::abs(i - best.x) > layout.row_step / 2)
      runner = std::max(runner, static_cast<double>(average.at<float>(0, i)));
  int y = top + best.x;
  AnchorMatch match{score >= 0.9 && weakest >= 0.8 && score - runner >= 0.05,
                    y,
                    anchor.y - y,
                    score,
                    weakest,
                    runner,
                    raw_score,
                    raw_weakest};
  int columns = static_cast<int>(layout.columns.size());
  for (int row = 0; row < raw_scores.rows; ++row) {
    const float* raw = raw_scores.ptr<float>(row);
    int local = best.x;
    // 同分时保留0偏移；只有局部匹配分数确实提高才记录校正。
    for (int offset : {-1, 1}) {
      int i = best.x + offset;
      if (i >= 0 && i < raw_scores.cols && raw[i] > raw[local]) local = i;
    }
    match.samples.push_back(
        {row / columns + 1, row % columns + 1, local - best.x, raw[best.x], raw[local]});
  }
  return match;
}
auto inputs_stable(const Page& previous, const Page& current) -> bool {
  if (previous.rows.size() != current.rows.size() ||
      previous.bottom.at_bottom != current.bottom.at_bottom ||
      previous.single_page != current.single_page || previous.names.size() != current.names.size())
    return false;
  for (std::size_t i = 0; i < previous.rows.size(); ++i)
    if (previous.rows[i].y != current.rows[i].y ||
        previous.rows[i].cards.size() != current.rows[i].cards.size())
      return false;
  for (std::size_t i = 0; i < previous.names.size(); ++i)
    if (!difference_ok(previous.names[i], current.names[i], 1) ||
        !difference_ok(previous.stars[i], current.stars[i], 2))
      return false;
  return true;
}
auto ownership(Item& item, const cv::Mat& star, const OwnershipSettings& settings) -> void {
  auto pixels = star_pixels(star, settings);
  auto mean = cv::mean(pixels);
  item.star_luma = mean[0] * 0.114 + mean[1] * 0.587 + mean[2] * 0.299;
  bool owned = std::abs(item.star_luma - settings.owned_luma) <= settings.max_luma_error;
  bool unowned = std::abs(item.star_luma - settings.unowned_luma) <= settings.max_luma_error;
  item.ownership = owned != unowned ? (owned ? "owned" : "unowned") : "unknown";
}
}  // namespace features::scan

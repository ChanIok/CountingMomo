#include "features/scan/vision.hpp"

#include "vendor/std.hpp"

#include "core/message/message.hpp"

#include "features/scan/layout.hpp"
#include "utils/image.hpp"
#include "utils/string.hpp"

namespace features::scan {

namespace {

auto quantile(std::vector<float> values, double fraction) -> double {
  if (values.empty()) {
    return 0;
  }
  std::ranges::sort(values);
  double position = fraction * (values.size() - 1);
  auto lower_index = static_cast<std::size_t>(position);
  auto upper_index = std::min(lower_index + 1, values.size() - 1);
  return values[lower_index] +
         (values[upper_index] - values[lower_index]) * (position - lower_index);
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
    for (int row = 0; row < line.rows; ++row) {
      for (int column = 0; column < line.cols; ++column) {
        pixels.push_back(line.at<std::uint8_t>(row, column));
      }
    }
    profile.push_back(static_cast<float>(quantile(std::move(pixels), 0.5)));
  }
  double first_edge_luma = (profile[0] + profile[1]) * 0.5;
  double last_edge_luma = (profile[length - 2] + profile[length - 1]) * 0.5;
  return *std::max_element(profile.begin() + 2, profile.end() - 2) -
         std::min(first_edge_luma, last_edge_luma);
}

// 只提取原生掩码中的固定前景像素，不缩放，也不混入星星背景。
auto star_pixels(const cv::Mat& star, const OwnershipSettings& settings) -> cv::Mat {
  cv::Mat pixels;
  for (int y = 0; y < star.rows; ++y) {
    for (int x = 0; x < star.cols; ++x) {
      if (settings.foreground_mask[y][x] != '1') {
        continue;
      }
      auto color = star.at<cv::Vec3b>(y, x);
      cv::Mat pixel(1, 1, CV_32FC3, cv::Scalar(color[0], color[1], color[2]));
      pixels.push_back(pixel);
    }
  }
  return pixels;
}

auto scrollbar_visible(const cv::Mat& image, const Layout& layout) -> bool {
  const auto& settings = layout.bottom_detection;
  int top = layout.safe_y[0];
  int bottom = settings.line_rect[3];
  int window_height = settings.line_rect[3] - settings.line_rect[1];
  std::array<Box, 3> boxes{settings.line_rect, settings.left_rect, settings.right_rect};
  std::array<cv::Mat, 3> bands;
  for (int i = 0; i < 3; ++i) {
    bands[i] = image(cv::Rect(boxes[i][0], top, boxes[i][2] - boxes[i][0], bottom - top));
  }
  std::vector<int> clear_rows;
  int clear_count = 0;
  for (int y = 0; y < bottom - top; ++y) {
    std::array<double, 3> means;
    for (int i = 0; i < 3; ++i) {
      auto mean = cv::mean(bands[i].row(y));
      means[i] = (mean[0] + mean[1] + mean[2]) / 3;
    }
    double contrast = means[0] - (means[1] + means[2]) / 2;
    clear_rows.push_back(contrast >= settings.min_contrast ? 1 : 0);
    clear_count += clear_rows.back();
    if (y >= window_height) {
      clear_count -= clear_rows[y - window_height];
    }
    if (y + 1 >= window_height &&
        static_cast<double>(clear_count) / window_height >= settings.min_clear_fraction) {
      return true;
    }
  }
  return false;
}

auto difference_ok(const cv::Mat& first, const cv::Mat& second, double mean_limit) -> bool {
  if (first.size() != second.size() || first.type() != second.type()) {
    return false;
  }
  cv::Mat first_float;
  cv::Mat second_float;
  cv::Mat difference;
  first.convertTo(first_float, CV_32F);
  second.convertTo(second_float, CV_32F);
  cv::absdiff(first_float, second_float, difference);
  auto flat = difference.reshape(1, 1);
  if (cv::mean(flat)[0] > mean_limit) {
    return false;
  }
  std::vector<float> values(flat.ptr<float>(), flat.ptr<float>() + flat.total());
  return quantile(std::move(values), 0.99) <= 8;
}

}  // namespace

auto load_hidden_question(const Layout& layout, const std::filesystem::path& layout_path)
    -> std::expected<cv::Mat, core::Error> {
  auto image =
      utils::image::load(layout_path.parent_path() / utils::string::wide(layout.hidden_card.image));
  if (!image) {
    return std::unexpected(image.error());
  }
  auto area = rect(layout.hidden_card.search_rect);
  if (image->cols > area.width || image->rows > area.height) {
    return std::unexpected(core::error("error.vision.question_template_too_large"));
  }
  cv::Mat gray;
  cv::cvtColor(*image, gray, cv::COLOR_BGR2GRAY);
  cv::Scalar mean;
  cv::Scalar deviation;
  cv::meanStdDev(gray, mean, deviation);
  if (deviation[0] < 3) {
    return std::unexpected(core::error("error.vision.empty_question_template"));
  }
  return gray;
}

auto items_at_top(const cv::Mat& image, const Layout& layout) -> bool {
  if (!scrollbar_visible(image, layout)) {
    return true;
  }
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
    if (contrast >= settings.min_contrast) {
      ++clear;
    }
  }
  double fraction = static_cast<double>(clear) / contrasts.size();
  return {fraction >= settings.min_clear_fraction, fraction, quantile(std::move(contrasts), 0.5)};
}

auto read_page(const cv::Mat& image, const Layout& layout, const cv::Mat& hidden_question,
               int phase, bool at_top, BottomResult bottom) -> std::expected<Page, core::Error> {
  if (image.cols != layout.reference_size[0] || image.rows != layout.reference_size[1] ||
      image.type() != CV_8UC3) {
    return std::unexpected(core::error("error.vision.capture_size_mismatch"));
  }
  Page page;
  page.image = image;
  page.bottom = bottom;
  // 首屏没有滚动条时，按整个分类已在一屏内展示处理。
  page.single_page = at_top && !bottom.at_bottom && !scrollbar_visible(image, layout);
  const auto& columns = page.single_page ? layout.columns_without_scrollbar : layout.columns;
  cv::cvtColor(image, page.gray, cv::COLOR_BGR2GRAY);
  int readable_top = at_top ? layout.safe_y[0] : layout.scrolled_safe_top;
  int readable_bottom = bottom.at_bottom ? layout.bottom_detection.safe_bottom : layout.safe_y[1];
  int row_step = layout.row_step;
  int card_height = layout.card_size[1];
  phase = (phase % row_step + row_step) % row_step;
  page.phase = phase;
  int first_row_y =
      phase +
      static_cast<int>(std::ceil(static_cast<double>(readable_top - phase) / row_step)) * row_step;
  bool empty_tail = false;
  for (int y = first_row_y; y + card_height <= readable_bottom; y += row_step) {
    Row row{.y = y};
    for (int column = 0; column < static_cast<int>(columns.size()); ++column) {
      int x = columns[column];
      Sample sample;
      sample.column = column + 1;
      sample.name = page.gray(rect(shifted(layout.name_tracking_rect, x, y)));
      auto question = page.gray(rect(shifted(layout.hidden_card.search_rect, x, y)));
      cv::Mat response;
      cv::matchTemplate(question, hidden_question, response, cv::TM_CCOEFF_NORMED);
      double score{};
      cv::minMaxLoc(response, nullptr, &score);
      sample.hidden = std::isfinite(score) && score >= layout.hidden_card.min_score;
      if (!sample.hidden) {
        bool bottom_edge = frame_contrast(page.gray, x, y, layout.card_frame.bottom_rect, true) >=
                           layout.card_frame.min_contrast;
        bool right_edge = frame_contrast(page.gray, x, y, layout.card_frame.right_rect, false) >=
                          layout.card_frame.min_contrast;
        if (bottom_edge != right_edge) {
          return std::unexpected(core::error("error.vision.border_not_ready", y, column + 1));
        }
        if (!bottom_edge) {
          if (!bottom.at_bottom && !page.single_page) {
            return std::unexpected(core::error("error.vision.border_missing", y, column + 1));
          }
          empty_tail = true;
          continue;
        }
      }
      // 问号本身就是明确占位证据，不要求其具有普通卡片的白色边框。
      if (empty_tail) {
        return std::unexpected(core::error("error.vision.gap_in_grid"));
      }
      if (sample.hidden) {
        sample.owned = 0;
        sample.signal = question;
      } else {
        cv::Scalar mean;
        cv::Scalar deviation;
        cv::meanStdDev(sample.name, mean, deviation);
        if (deviation[0] < 3) {
          return std::unexpected(core::error("error.vision.content_not_ready", y, column + 1));
        }
        sample.signal =
            star_pixels(image(rect(shifted(layout.star_rect, x, y))), layout.ownership_star);
        auto color = cv::mean(sample.signal);
        sample.star_luma = color[0] * 0.114 + color[1] * 0.587 + color[2] * 0.299;
        const auto& settings = layout.ownership_star;
        bool owned = std::abs(sample.star_luma - settings.owned_luma) <= settings.max_luma_error;
        bool unowned =
            std::abs(sample.star_luma - settings.unowned_luma) <= settings.max_luma_error;
        if (owned == unowned) {
          return std::unexpected(core::error("error.vision.star_not_ready", y, column + 1,
                                             std::format("{:.2f}", sample.star_luma)));
        }
        sample.owned = owned ? 1 : 0;
      }
      row.cards.push_back(std::move(sample));
    }
    if (!row.cards.empty()) {
      page.rows.push_back(std::move(row));
    }
  }
  if (page.rows.empty()) {
    return std::unexpected(core::error("error.vision.no_readable_rows"));
  }
  return page;
}

auto make_anchor(const Page& page, const Layout& layout, int overlap_rows)
    -> std::expected<Anchor, core::Error> {
  if (overlap_rows < 2 || static_cast<int>(page.rows.size()) < overlap_rows) {
    return std::unexpected(core::error("error.vision.not_enough_anchor_rows"));
  }
  Anchor anchor;
  int start = static_cast<int>(page.rows.size()) - overlap_rows;
  anchor.y = page.rows[start].y;
  anchor.span_rows = std::min(3, overlap_rows);
  for (int i = start; i < start + anchor.span_rows; ++i) {
    if (page.rows[i].cards.size() != layout.columns.size()) {
      return std::unexpected(core::error("error.vision.gap_in_anchor_row"));
    }
    for (const auto& card : page.rows[i].cards) {
      // 问号卡占位保留，但空白文字不参与接续。
      if (!card.hidden) {
        anchor.templates.push_back({i - start, card.column - 1, card.name.clone()});
      }
    }
  }
  if (anchor.templates.empty()) {
    return std::unexpected(core::error("error.vision.no_anchors"));
  }
  return anchor;
}

auto locate_anchor(const cv::Mat& image, const Anchor& anchor, const Layout& layout, bool at_bottom)
    -> AnchorMatch {
  cv::Mat gray;
  cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
  int top = layout.scrolled_safe_top;
  int bottom = at_bottom ? layout.bottom_detection.safe_bottom : layout.safe_y[1];
  int last = bottom - layout.card_size[1] - (anchor.span_rows - 1) * layout.row_step;
  if (last < top) {
    return {};
  }
  cv::Mat raw_scores;
  for (const auto& sample : anchor.templates) {
    const auto& name = sample.pixels;
    int left = layout.columns[sample.column] + layout.name_tracking_rect[0];
    int offset = layout.name_tracking_rect[1] + sample.row * layout.row_step;
    cv::Mat response;
    cv::matchTemplate(gray(cv::Rect(left, 0, name.cols, gray.rows)), name, response,
                      cv::TM_CCOEFF_NORMED);
    for (int y = 0; y < response.rows; ++y) {
      if (!std::isfinite(response.at<float>(y, 0))) {
        response.at<float>(y, 0) = -1;
      }
    }
    cv::Mat row_scores = response.rowRange(top + offset, last + offset + 1).t();
    raw_scores.push_back(row_scores);
  }
  // 整体位置始终取严格对齐的联合峰值，局部容差不改变位移或页面行相位。
  cv::Mat raw_average;
  cv::reduce(raw_scores, raw_average, 0, cv::REDUCE_AVG, CV_32F);
  cv::Point best;
  double raw_score{};
  cv::minMaxLoc(raw_average, nullptr, &raw_score, nullptr, &best);
  double raw_weakest_score{};
  cv::minMaxLoc(raw_scores.col(best.x), &raw_weakest_score, nullptr);

  // 每张名称仅在共同位置附近校正±1px；复用响应，不重做模板匹配或越过搜索边界。
  cv::Mat scores = raw_scores.clone();
  for (int row = 0; row < scores.rows; ++row) {
    const float* raw = raw_scores.ptr<float>(row);
    float* aligned = scores.ptr<float>(row);
    for (int i = 0; i < scores.cols; ++i) {
      if (i > 0) {
        aligned[i] = std::max(aligned[i], raw[i - 1]);
      }
      if (i + 1 < scores.cols) {
        aligned[i] = std::max(aligned[i], raw[i + 1]);
      }
    }
  }
  cv::Mat average;
  cv::reduce(scores, average, 0, cv::REDUCE_AVG, CV_32F);
  double score = average.at<float>(0, best.x);
  double weakest{};
  cv::minMaxLoc(scores.col(best.x), &weakest, nullptr);
  double runner_up_score = -1;
  for (int i = 0; i < average.cols; ++i) {
    if (std::abs(i - best.x) > layout.row_step / 2) {
      runner_up_score = std::max(runner_up_score, static_cast<double>(average.at<float>(0, i)));
    }
  }
  int y = top + best.x;
  AnchorMatch match{
      .valid = score >= 0.9 && weakest >= 0.8 && score - runner_up_score >= 0.05,
      .y = y,
      .displacement = anchor.y - y,
      .score = score,
      .weakest_score = weakest,
      .runner_up = runner_up_score,
      .raw_score = raw_score,
      .raw_weakest_score = raw_weakest_score,
  };
  for (int row = 0; row < raw_scores.rows; ++row) {
    const float* raw = raw_scores.ptr<float>(row);
    int local = best.x;
    // 同分时保留0偏移；只有局部匹配分数确实提高才记录校正。
    for (int offset : {-1, 1}) {
      int i = best.x + offset;
      if (i >= 0 && i < raw_scores.cols && raw[i] > raw[local]) {
        local = i;
      }
    }
    match.samples.push_back({
        .row = anchor.templates[row].row + 1,
        .column = anchor.templates[row].column + 1,
        .vertical_offset = local - best.x,
        .raw_score = raw[best.x],
        .score = raw[local],
    });
  }
  return match;
}

auto inputs_stable(const Page& previous, const Page& current) -> bool {
  if (previous.rows.size() != current.rows.size() ||
      previous.bottom.at_bottom != current.bottom.at_bottom ||
      previous.single_page != current.single_page) {
    return false;
  }
  for (std::size_t i = 0; i < previous.rows.size(); ++i) {
    const auto& previous_row = previous.rows[i];
    const auto& current_row = current.rows[i];
    if (previous_row.y != current_row.y || previous_row.cards.size() != current_row.cards.size()) {
      return false;
    }
    for (std::size_t j = 0; j < previous_row.cards.size(); ++j) {
      const auto& previous_card = previous_row.cards[j];
      const auto& current_card = current_row.cards[j];
      if (previous_card.column != current_card.column ||
          previous_card.hidden != current_card.hidden ||
          previous_card.owned != current_card.owned ||
          !difference_ok(previous_card.name, current_card.name, 1) ||
          !difference_ok(previous_card.signal, current_card.signal, 2)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace features::scan

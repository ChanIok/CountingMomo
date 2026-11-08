#include "features/scan/pages/vision.hpp"

#include "vendor/std.hpp"

#include "core/message/message.hpp"
#include "features/scan/layout.hpp"
#include "utils/image.hpp"

namespace features::scan::pages {

namespace {

constexpr Box card_bottom_rect{16, 60, 60, 70};
constexpr Box card_right_rect{62, 20, 71, 60};
constexpr double card_min_contrast = 20.0;
constexpr Box hidden_search_rect{26, 21, 48, 55};
constexpr double hidden_min_score = 0.9;
constexpr double content_min_deviation = 3.0;
constexpr Box star_rect{55, 7, 67, 21};
// 最右侧星星的核心前景，按 y±1px 位移稳定性贪心选出，排除左侧相邻星星与右侧白色边框；
// 坐标相对 star_rect。975 张 960 原生卡片 × 3 种位移全部正确分类。
constexpr std::array<std::array<int, 2>, 8> star_points{{
    {7, 5},
    {5, 6},
    {6, 6},
    {7, 6},
    {8, 6},
    {6, 7},
    {7, 7},
    {5, 8},
}};
// 已拥有与未拥有亮度来自 960 原生截图标定，容差区间之间保留空隙。
constexpr double owned_luma = 211.0;
constexpr double unowned_luma = 171.0;
constexpr double max_luma_error = 16.0;
constexpr std::array<Box, 3> scrollbar_bands{{
    {932, 3720, 937, 3776},
    {924, 3720, 929, 3776},
    {940, 3720, 945, 3776},
}};
constexpr double scrollbar_min_contrast = 25.0;
constexpr double scrollbar_min_clear_fraction = 0.9;
constexpr int max_anchor_rows = 3;
constexpr double anchor_min_score = 0.9;
constexpr double anchor_min_sample_score = 0.8;
constexpr double anchor_min_margin = 0.05;

// 计算浮点值向量的分位数
auto quantile(std::vector<float> values, double fraction) -> double {
  if (values.empty()) {
    return 0;
  }
  // 升序排序输入序列
  std::ranges::sort(values);
  // 计算插值位置
  double position = fraction * (values.size() - 1);
  auto lower_index = static_cast<std::size_t>(position);
  auto upper_index = std::min(lower_index + 1, values.size() - 1);
  return values[lower_index] +
         (values[upper_index] - values[lower_index]) * (position - lower_index);
}

// 将矩形按偏移量移动
auto shifted(Box box, int x, int y) -> Box {
  return {box[0] + x, box[1] + y, box[2] + x, box[3] + y};
}

// 计算卡片边框采样带的灰度边缘对比度
auto frame_contrast(const cv::Mat& gray, int x, int y, Box box, bool horizontal) -> double {
  // 截取卡片局部边框感兴趣区域
  auto roi = gray(rect(shifted(box, x, y)));
  std::vector<float> profile;
  int length = horizontal ? roi.rows : roi.cols;
  // 逐线统计中位数像素亮度，构造边缘亮度剖面线
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
  // 计算剖面线内部峰值与两侧边缘基准亮度的对比落差
  double first_edge_luma = (profile[0] + profile[1]) * 0.5;
  double last_edge_luma = (profile[length - 2] + profile[length - 1]) * 0.5;
  return *std::max_element(profile.begin() + 2, profile.end() - 2) -
         std::min(first_edge_luma, last_edge_luma);
}

// 只取标定前景像素，避免背景和相邻星星混入拥有状态亮度。
auto star_pixels(const cv::Mat& star) -> cv::Mat {
  cv::Mat pixels;
  for (auto point : star_points) {
    auto color = star.at<cv::Vec3b>(point[1], point[0]);
    cv::Mat pixel(1, 1, CV_32FC3, cv::Scalar(color[0], color[1], color[2]));
    pixels.push_back(pixel);
  }
  return pixels;
}

// 检查滑动条采样带对比度以判断是否存在垂直滚动条
auto scrollbar_visible(const cv::Mat& image, const Layout& layout) -> bool {
  int top = layout.safe_y[0];
  int bottom = scrollbar_bands[0][3];
  int window_height = scrollbar_bands[0][3] - scrollbar_bands[0][1];
  const auto& boxes = scrollbar_bands;
  std::array<cv::Mat, 3> bands;
  // 提取中心滑道、左侧背景与右侧背景三条纵向采样带
  for (int i = 0; i < 3; ++i) {
    bands[i] = image(cv::Rect(boxes[i][0], top, boxes[i][2] - boxes[i][0], bottom - top));
  }
  std::vector<int> clear_rows;
  int clear_count = 0;
  // 沿纵向滑窗分析滑块与背景的对比度，识别滚动条轨迹
  for (int y = 0; y < bottom - top; ++y) {
    std::array<double, 3> means;
    for (int i = 0; i < 3; ++i) {
      auto mean = cv::mean(bands[i].row(y));
      means[i] = (mean[0] + mean[1] + mean[2]) / 3;
    }
    double contrast = means[0] - (means[1] + means[2]) / 2;
    clear_rows.push_back(contrast >= scrollbar_min_contrast ? 1 : 0);
    clear_count += clear_rows.back();
    if (y >= window_height) {
      clear_count -= clear_rows[y - window_height];
    }
    // 当窗口范围内对比度达标行占比超过阈值时判定存在滚动条
    if (y + 1 >= window_height &&
        static_cast<double>(clear_count) / window_height >= scrollbar_min_clear_fraction) {
      return true;
    }
  }
  return false;
}

}  // namespace

// 加载问号模板素材：读取图片 → 转灰度 → 校验方差有效性
auto load_hidden_question(const std::filesystem::path& path)
    -> std::expected<cv::Mat, core::Error> {
  // 从磁盘读取问号模板图像
  auto image = utils::image::load(path);
  if (!image) {
    return std::unexpected(image.error());
  }
  // 确保模板未超过卡片预留的搜索框范围
  auto area = rect(hidden_search_rect);
  if (image->cols > area.width || image->rows > area.height) {
    return std::unexpected(core::error("error.vision.question_template_too_large"));
  }
  cv::Mat gray;
  cv::cvtColor(*image, gray, cv::COLOR_BGR2GRAY);
  cv::Scalar mean;
  cv::Scalar deviation;
  // 校验模板图案具有足够的对比方差，避免空白图
  cv::meanStdDev(gray, mean, deviation);
  if (deviation[0] < content_min_deviation) {
    return std::unexpected(core::error("error.vision.empty_question_template"));
  }
  return gray;
}

// 检测列表是否触底：分析滚动条底部三条采样带对比度并计算清晰度比例
auto detect_bottom(const cv::Mat& image) -> BottomResult {
  std::array<std::vector<double>, 3> regions;
  const auto& boxes = scrollbar_bands;
  // 采样底部滑块区域及左右背景线
  for (int i = 0; i < 3; ++i) {
    auto roi = image(rect(boxes[i]));
    for (int y = 0; y < roi.rows; ++y) {
      auto mean = cv::mean(roi.row(y));
      regions[i].push_back((mean[0] + mean[1] + mean[2]) / 3);
    }
  }
  std::vector<float> contrasts;
  int clear = 0;
  // 逐行统计高对比度像素行比例
  for (std::size_t y = 0; y < regions[0].size(); ++y) {
    double contrast = regions[0][y] - (regions[1][y] + regions[2][y]) / 2;
    contrasts.push_back(static_cast<float>(contrast));
    if (contrast >= scrollbar_min_contrast) {
      ++clear;
    }
  }
  double fraction = static_cast<double>(clear) / contrasts.size();
  return {fraction >= scrollbar_min_clear_fraction, fraction, quantile(std::move(contrasts), 0.5)};
}

// 读取单页可见卡片拥有状态：尺寸校验 → 遍历安全区网格行与列 → 识别问号卡与边框 →
// 提取星星并计算亮度得出 0/1 拥有值
auto read_page(const cv::Mat& image, const Layout& layout, const cv::Mat& hidden_question,
               int phase, bool first_page, BottomResult bottom)
    -> std::expected<Page, core::Error> {
  // 严格校验捕获图像尺寸与色彩空间类型
  if (image.cols != layout.reference_size[0] || image.rows != layout.reference_size[1] ||
      image.type() != CV_8UC3) {
    return std::unexpected(core::error("error.vision.capture_size_mismatch"));
  }
  Page page;
  page.image = image;
  page.bottom = bottom;
  // 首屏没有滚动条时，按整个分类已在一屏内展示处理。
  page.single_page = first_page && !bottom.at_bottom && !scrollbar_visible(image, layout);
  // 单屏布局与带滚动条布局的列坐标略有差异，在此切换对应列位置表
  const auto& columns = page.single_page ? layout.columns_without_scrollbar : layout.columns;
  // 转换为灰度图供边框与模板匹配使用
  cv::cvtColor(image, page.gray, cv::COLOR_BGR2GRAY);
  int readable_top = first_page ? layout.safe_y[0] : layout.scrolled_safe_top;
  int readable_bottom = bottom.at_bottom ? layout.bottom_safe_y : layout.safe_y[1];
  int row_step = layout.row_step;
  int card_height = layout.card_size[1];
  // 计算当前滚动相位对齐的首行 Y 坐标
  phase = (phase % row_step + row_step) % row_step;
  page.phase = phase;
  int first_row_y =
      phase +
      static_cast<int>(std::ceil(static_cast<double>(readable_top - phase) / row_step)) * row_step;
  bool empty_tail = false;
  // 按行高步进遍历安全视野内的所有完整行
  for (int y = first_row_y; y + card_height <= readable_bottom; y += row_step) {
    Row row{.y = y};
    // 逐列读取卡片
    for (int column = 0; column < static_cast<int>(columns.size()); ++column) {
      int x = columns[column];
      Sample sample;
      sample.column = column + 1;
      // 提取文字区域灰度图用于接续追踪（不作 OCR）
      sample.name = page.gray(rect(shifted(layout.name_tracking_rect, x, y)));
      auto question = page.gray(rect(shifted(hidden_search_rect, x, y)));
      // 与问号模板执行归一化相关性匹配
      cv::Mat response;
      cv::matchTemplate(question, hidden_question, response, cv::TM_CCOEFF_NORMED);
      double score{};
      cv::minMaxLoc(response, nullptr, &score);
      // 判定是否为未解禁的问号卡片
      sample.hidden = std::isfinite(score) && score >= hidden_min_score;
      if (!sample.hidden) {
        // 普通卡片校验右侧和底侧白色高亮边框是否渲染就绪
        bool bottom_edge =
            frame_contrast(page.gray, x, y, card_bottom_rect, true) >= card_min_contrast;
        bool right_edge =
            frame_contrast(page.gray, x, y, card_right_rect, false) >= card_min_contrast;
        // 两条边框就绪状态不一致说明处于滚动动效或未完全加载中，报错等待重采样
        if (bottom_edge != right_edge) {
          return std::unexpected(core::error("error.vision.border_not_ready", y, column + 1));
        }
        // 若边框缺失且非列表末尾空白，报错中断
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
        // 问号卡片确定未拥有，值记为 0
        sample.owned = 0;
      } else {
        cv::Scalar mean;
        cv::Scalar deviation;
        // 校验卡片文字区域是否有对比度（确保不是空白未加载纹理）
        cv::meanStdDev(sample.name, mean, deviation);
        if (deviation[0] < content_min_deviation) {
          return std::unexpected(core::error("error.vision.content_not_ready", y, column + 1));
        }
        // 提取右下角星星的前景像素并计算加权平均亮度
        auto pixels = star_pixels(image(rect(shifted(star_rect, x, y))));
        auto color = cv::mean(pixels);
        sample.star_luma = color[0] * 0.114 + color[1] * 0.587 + color[2] * 0.299;
        // 与已拥有和未拥有的亮度标准区间进行比对
        bool owned = std::abs(sample.star_luma - owned_luma) <= max_luma_error;
        bool unowned = std::abs(sample.star_luma - unowned_luma) <= max_luma_error;
        // 亮度落在灰色过渡地带或两者皆不符，报错等待新帧
        if (owned == unowned) {
          return std::unexpected(core::error("error.vision.star_not_ready", y, column + 1,
                                             std::format("{:.2f}", sample.star_luma)));
        }
        // 赋予 0/1 业务值
        sample.owned = owned ? 1 : 0;
      }
      row.cards.push_back(std::move(sample));
    }
    if (!row.cards.empty()) {
      page.rows.push_back(std::move(row));
    }
  }
  // 安全区内没有任何可读行则报错
  if (page.rows.empty()) {
    return std::unexpected(core::error("error.vision.no_readable_rows"));
  }
  return page;
}

// 从页面尾部提取文字模板作为下一页对齐锚点：选取重叠行 → 克隆普通卡文字灰度图
auto make_anchor(const Page& page, const Layout& layout, int overlap_rows)
    -> std::expected<Anchor, core::Error> {
  // 检查是否有足够的重叠行
  if (overlap_rows < 2 || static_cast<int>(page.rows.size()) < overlap_rows) {
    return std::unexpected(core::error("error.vision.not_enough_anchor_rows"));
  }
  Anchor anchor;
  int start = static_cast<int>(page.rows.size()) - overlap_rows;
  anchor.y = page.rows[start].y;
  anchor.span_rows = std::min(max_anchor_rows, overlap_rows);
  // 遍历重叠行收集锚点文字样本
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
  // 确保重叠区域内至少存在有效文字卡片
  if (anchor.templates.empty()) {
    return std::unexpected(core::error("error.vision.no_anchors"));
  }
  return anchor;
}

// 模板匹配计算页面滚动位移：多采样文字模板匹配 → 联合响应峰定位整体像素位移 → 局部容差微调
auto locate_anchor(const cv::Mat& image, const Anchor& anchor, const Layout& layout, bool at_bottom)
    -> AnchorMatch {
  cv::Mat gray;
  cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
  int top = layout.scrolled_safe_top;
  int bottom = at_bottom ? layout.bottom_safe_y : layout.safe_y[1];
  int last = bottom - layout.card_size[1] - (anchor.span_rows - 1) * layout.row_step;
  if (last < top) {
    return {};
  }
  cv::Mat raw_scores;
  // 对所有锚点文字样本在对应列的纵向采样带内执行模板匹配
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
  cv::minMaxLoc(raw_average, nullptr, nullptr, nullptr, &best);

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
  // 寻找远离最佳峰值处的次优峰，计算峰值尖锐度差
  for (int i = 0; i < average.cols; ++i) {
    if (std::abs(i - best.x) > layout.row_step / 2) {
      runner_up_score = std::max(runner_up_score, static_cast<double>(average.at<float>(0, i)));
    }
  }
  int y = top + best.x;
  // 校验峰值分数是否达到高置信度要求
  return {
      .valid = score >= anchor_min_score && weakest >= anchor_min_sample_score &&
               score - runner_up_score >= anchor_min_margin,
      .displacement = anchor.y - y,
  };
}

}  // namespace features::scan::pages

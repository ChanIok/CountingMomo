#include "features/scan/menu/vision.hpp"

#include "vendor/std.hpp"

#include "vendor/opencv.hpp"

#include "core/state/app_state.hpp"
#include "features/scan/layout.hpp"
#include "features/scan/menu/constants.hpp"
#include "features/scan/state.hpp"

namespace features::scan::menu {

namespace {

constexpr double match_threshold = 0.8;
constexpr double match_margin = 0.05;
constexpr double selection_contrast = 3.0;
constexpr double signal_blur_sigma = 2.0;
constexpr int layout_min_anchors = 2;
constexpr double layout_highlight_contrast = 20.0;
constexpr double layout_highlight_ratio = 1.5;
constexpr int child_origin_min_anchors = 2;

}  // namespace

// 将 BGR 图像转换为单通道灰度图
auto gray(const cv::Mat& image) -> cv::Mat {
  cv::Mat result;
  cv::cvtColor(image, result, cv::COLOR_BGR2GRAY);
  return result;
}

// 将图像转换为 32 位浮点灰度图，供高亮对比度采样复用
auto gray_float(const cv::Mat& pixels) -> cv::Mat {
  cv::Mat result;
  gray(pixels).convertTo(result, CV_32F);
  return result;
}

// 提取图标高频边缘特征信号：原图减去高斯模糊低频图
auto icon_signal(const cv::Mat& image) -> cv::Mat {
  cv::Mat value;
  cv::Mat blurred;
  cv::Mat result;
  // 转为浮点灰度并做高斯模糊
  gray(image).convertTo(value, CV_32F);
  cv::GaussianBlur(value, blurred, {0, 0}, signal_blur_sigma);
  // 差分提取高频纹理并截断负值
  cv::subtract(value, blurred, result);
  cv::max(result, 0, result);
  return result;
}

// 在指定区域内通过模板匹配定位图标中心：特征差分 → 相关性匹配 → 峰值筛选
auto locate(const cv::Mat& image, const Icon& icon, const Box& bounds, bool child)
    -> std::optional<Match> {
  // 裁剪搜索区并提取高频边缘特征
  auto query = icon_signal(image(scan::rect(bounds)));
  int width = icon.signal.cols;
  int height = icon.signal.rows;
  if (query.cols < width || query.rows < height) {
    return {};
  }
  // 执行归一化相关性模板匹配
  cv::Mat response;
  cv::matchTemplate(query, icon.signal, response, cv::TM_CCOEFF_NORMED);
  // 将无纹理产生的非法值置为 -1
  for (int y = 0; y < response.rows; ++y) {
    for (int x = 0; x < response.cols; ++x) {
      if (!std::isfinite(response.at<float>(y, x))) {
        response.at<float>(y, x) = -1;
      }
    }
  }
  double score{};
  cv::Point best;
  cv::minMaxLoc(response, nullptr, &score, nullptr, &best);
  // 抹除最佳峰值邻域并求取次峰得分，检验峰值尖锐度
  int left = std::max(0, best.x - width / 2);
  int top = std::max(0, best.y - height / 2);
  int right = std::min(response.cols, best.x + width / 2 + 1);
  int bottom = std::min(response.rows, best.y + height / 2 + 1);
  response(cv::Rect(left, top, right - left, bottom - top)).setTo(-1);
  double second{};
  cv::minMaxLoc(response, nullptr, &second);
  // 换算图标在客户区中的中心绝对坐标
  cv::Point point = cv::Point(bounds[0], bounds[1]) + best +
                    cv::Point(icon.definition->size / 2, icon.definition->size / 2);
  const auto& vertical = child ? bounds : menu_rect;
  int half = child ? child_half_size : root_half_size;
  // 校验匹配置信度、峰值边距及纵向有效边界
  if (score < match_threshold || score - second < match_margin || point.y - half < vertical[1] ||
      point.y + half >= vertical[3]) {
    return {};
  }
  return Match{point};
}

// 采样图标四边缘的高光边框对比度
auto contrast(const cv::Mat& image, cv::Point point, bool child) -> double {
  int half = child ? child_half_size : root_half_size;
  int span = half - selection_corner_inset;
  // 计算单条边缘采样带的高光峰值
  auto edge = [&](cv::Rect area, int dimension) {
    cv::Mat values;
    cv::reduce(image(area), values, dimension, cv::REDUCE_AVG, CV_32F);
    auto* data = values.ptr<float>();
    double best = -std::numeric_limits<double>::infinity();
    for (int i = selection_contrast_radius;
         i < 2 * selection_band_half + 1 - selection_contrast_radius; ++i) {
      best = std::max(best, static_cast<double>(data[i] - (data[i - selection_contrast_radius] +
                                                           data[i + selection_contrast_radius]) *
                                                              0.5f));
    }
    return best;
  };
  // 返回四条边中高光对比度的最小值
  return std::min({edge({point.x - span, point.y - half - selection_band_half, span * 2 + 1,
                         2 * selection_band_half + 1},
                        1),
                   edge({point.x - span, point.y + half - selection_band_half, span * 2 + 1,
                         2 * selection_band_half + 1},
                        1),
                   edge({point.x - half - selection_band_half, point.y - span,
                         2 * selection_band_half + 1, span * 2 + 1},
                        0),
                   edge({point.x + half - selection_band_half, point.y - span,
                         2 * selection_band_half + 1, span * 2 + 1},
                        0)});
}

// 在纵向微调窗口内采样最强的高光边框对比度响应
auto highlight_signal(const cv::Mat& flat, cv::Point point, bool child) -> double {
  int half = child ? child_half_size : root_half_size;
  int top = menu_rect[1] + half + selection_band_half;
  int bottom = menu_rect[3] - half - selection_band_half;
  double best = -std::numeric_limits<double>::infinity();
  // 在容差窗口内搜索最大对比度，补偿取整误差
  for (int dy = -layout_probe_radius; dy <= layout_probe_radius; ++dy) {
    int y = point.y + dy;
    if (y >= top && y < bottom) {
      best = std::max(best, contrast(flat, cv::Point{point.x, y}, child));
    }
  }
  return best;
}

// 按 ID 在目录中查找运行时图标对象
auto find_icon(const Catalog& catalog, std::string_view id) -> const Icon* {
  for (const auto& group : catalog.groups) {
    if (group.entry && group.entry->definition->id == id) {
      return &*group.entry;
    }
    for (const auto& icon : group.categories) {
      if (icon.definition->id == id) {
        return &icon;
      }
    }
  }
  return nullptr;
}

// 按 ID 在目录中查找运行时分组对象
auto find_group(const Catalog& catalog, std::string_view id) -> const Group* {
  for (const auto& group : catalog.groups) {
    if (group.definition->id == id) {
      return &group;
    }
  }
  return nullptr;
}

// 查找指定分类 ID 对应的外层行号
auto root_row_of(std::string_view id) -> int {
  for (int row = 0; row < static_cast<int>(root_ids.size()); ++row) {
    if (root_ids[row] == id) {
      return row;
    }
  }
  return -1;
}

// 获取指定外层行相对 hair 行的累计像素偏移
auto root_offset(int row, bool expanded) -> int {
  return expanded && row == static_cast<int>(root_offsets.size()) - 1 ? expanded_makeup_offset
                                                                      : root_offsets[row];
}

// 对多个候选原点投票：排序取中位数 → 统计容差内票数 → 票数充足返回原点
auto vote_origin(std::vector<int> origins, int min_anchors) -> std::optional<int> {
  if (static_cast<int>(origins.size()) < min_anchors) {
    return {};
  }
  // 排序并取中位数
  std::ranges::sort(origins);
  int median = origins[origins.size() / 2];
  // 统计落在容差内的有效票数
  int consistent = static_cast<int>(std::ranges::count_if(origins, [median](int value) {
    return std::abs(value - median) <= layout_anchor_tolerance;
  }));
  if (consistent < min_anchors) {
    return {};
  }
  return median;
}

// 检验父入口下方是否可见已展开的子项列表：匹配子项图标 → 投票原点
auto child_list_visible(const Group& group, const cv::Mat& image, int parent_y) -> bool {
  const int top = std::max(parent_y + child_top_offset, menu_rect[1]);
  const int viewport_bottom = parent_y + accessories_viewport_height - next_parent_bottom_inset -
                              group.definition->bottom_fade_inset;
  const int bottom = std::min(viewport_bottom, menu_rect[3]);
  // 视口高度不足以容纳至少两行子项
  if (bottom - top < child_list_probe_height) {
    return false;
  }
  const Box area{child_search_x[0], top, child_search_x[1], bottom};
  const int count = static_cast<int>(group.categories.size());
  std::vector<int> origins;
  // 依次匹配子项图标推算原点并投票
  for (int k = 0; k < count; ++k) {
    const int index = (k + 1) % count;
    if (auto match = locate(image, group.categories[index], area, true)) {
      origins.push_back(match->point.y - index * child_row_step);
      if (vote_origin(origins, child_origin_min_anchors)) {
        return true;
      }
    }
  }
  return false;
}

// 推算外层整列排布及当前选中行：定位可见图标 → 投票原点 → 量取高亮行
auto infer_root_layout(const core::AppState& state, const cv::Mat& image, const cv::Mat& flat)
    -> std::optional<RootLayout> {
  const auto& catalog = state.scan->session.menu.catalog;
  std::vector<std::pair<int, int>> anchors;
  // 搜索定位全部可见的根图标
  for (int row = 1; row < static_cast<int>(root_offsets.size()); ++row) {
    const auto* icon = find_icon(catalog, root_ids[row]);
    if (icon) {
      if (auto match = locate(image, *icon, root_search_rect)) {
        anchors.emplace_back(row, match->point.y);
      }
    }
  }
  if (anchors.size() < layout_min_anchors) {
    return {};
  }
  const int makeup_row = static_cast<int>(root_offsets.size()) - 1;
  std::vector<int> stable_origins;
  std::optional<int> makeup_y;
  // 分离不受展开状态影响的稳定行与受展开影响的妆容行
  for (const auto& [row, y] : anchors) {
    if (row == makeup_row) {
      makeup_y = y;
    } else {
      stable_origins.push_back(y - root_offsets[row]);
    }
  }
  int origin{};
  bool expanded{};
  // 优先用稳定行投票推算原点
  if (auto voted = vote_origin(std::move(stable_origins), layout_min_anchors)) {
    origin = *voted;
    const int accessories_row = root_row_of("accessories");
    if (const auto* accessories = find_group(catalog, "accessories");
        accessories && accessories_row >= 0) {
      expanded = child_list_visible(*accessories, image, origin + root_offsets[accessories_row]);
    }
    // 备选判据：若子列表未匹配成功，通过妆容行是否下移兜底判定展开态
    if (!expanded && makeup_y) {
      expanded = *makeup_y - origin > expanded_makeup_midpoint;
    }
  } else {
    // 稳定行不足时，分别按折叠与展开两种候选排布投票
    int best_count{};
    for (bool candidate : {false, true}) {
      std::vector<int> origins;
      for (const auto& [row, y] : anchors) {
        origins.push_back(y - root_offset(row, candidate));
      }
      std::ranges::sort(origins);
      int median = origins[origins.size() / 2];
      int count = static_cast<int>(std::ranges::count_if(origins, [median](int value) {
        return std::abs(value - median) <= layout_anchor_tolerance;
      }));
      if (count > best_count) {
        best_count = count;
        origin = median;
        expanded = candidate;
      }
    }
    if (best_count < layout_min_anchors) {
      return {};
    }
  }
  // 生成整列所有行的推算坐标
  RootLayout layout{.origin = origin, .expanded = expanded};
  for (int row = 0; row < static_cast<int>(root_offsets.size()); ++row) {
    layout.points[row] = cv::Point{outer_scroll_point[0], origin + root_offset(row, expanded)};
  }
  // 逐行检测高亮边框信号，找出最高亮行
  double best = layout_highlight_contrast;
  double second{};
  for (int row = 0; row < static_cast<int>(root_offsets.size()); ++row) {
    double value = highlight_signal(flat, layout.points[row], false);
    if (value > best) {
      second = best;
      best = value;
      layout.highlighted = row;
    } else if (value > second) {
      second = value;
    }
  }
  // 校验最高亮相比次高亮的优势比例，无明显赢家判为未选中
  if (layout.highlighted < 0 || best < second * layout_highlight_ratio) {
    layout.highlighted = -1;
  }
  return layout;
}

// 计算指定分组子项列表在当前排布下的可见搜索视口
auto bounds(const Group& group, const RootLayout& layout) -> std::optional<Box> {
  // 根分组直接使用默认搜索框
  if (!group.entry) {
    return root_search_rect;
  }
  int row = root_row_of(group.entry->definition->id);
  if (row < 0) {
    return {};
  }
  int parent_y = layout.points[row].y;
  int bottom;
  // 饰品分组以妆容行为底界，其他分组以固定视口偏移为底界
  if (group.definition->id == "accessories") {
    if (!layout.expanded) {
      return {};
    }
    bottom = layout.points[static_cast<int>(root_offsets.size()) - 1].y - next_parent_bottom_inset;
  } else {
    bottom = parent_y + group.definition->viewport_bottom_offset;
    if (bottom > menu_rect[3]) {
      return {};
    }
  }
  // 扣除底部渐变模糊区并校验视口最小高度
  bottom -= group.definition->bottom_fade_inset;
  if (bottom - (parent_y + child_top_offset) < child_min_height) {
    return {};
  }
  return Box{child_search_x[0], parent_y + child_top_offset, child_search_x[1], bottom};
}

// 获取分类在所属分组中的下标索引
auto category_index(const Group& group, const Icon& icon) -> int {
  auto found = std::ranges::find_if(group.categories, [&](const Icon& item) {
    return item.definition->id == icon.definition->id;
  });
  return static_cast<int>(found - group.categories.begin());
}

// 匹配所有可见子项推算子视口列表原点 y 坐标
auto child_origin(const Group& group, const cv::Mat& image, const Box& area) -> std::optional<int> {
  std::vector<int> origins;
  // 匹配可见子项并推算首项原点
  for (int i = 0; i < static_cast<int>(group.categories.size()); ++i) {
    if (auto match = locate(image, group.categories[i], area, true)) {
      origins.push_back(match->point.y - i * child_row_step);
    }
  }
  // 投票得出子视口原点
  return vote_origin(std::move(origins), child_origin_min_anchors);
}

// 推算目标分类或分组图标在当前画面中的中心坐标
auto target_point(const Group& group, const Icon& icon, const cv::Mat& image,
                  const RootLayout& layout, bool root) -> std::optional<cv::Point> {
  // 根分类或分组入口直接从外层排布读取
  if (!group.entry || root) {
    int row = root_row_of(icon.definition->id);
    if (row < 0) {
      return {};
    }
    return layout.points[row];
  }
  // 计算子视口搜索范围
  auto area = bounds(group, layout);
  if (!area) {
    return {};
  }
  // 推算子视口列表原点
  auto origin = child_origin(group, image, *area);
  if (!origin) {
    return {};
  }
  int index = category_index(group, icon);
  if (index >= static_cast<int>(group.categories.size())) {
    return {};
  }
  // 根据子项索引与行距计算绝对坐标
  return cv::Point{outer_scroll_point[0], *origin + index * child_row_step};
}

// 判定目标分类或分组在当前画面中是否处于选中高亮状态
auto selected(const core::AppState& state, const Group& group, const Icon& icon,
              const cv::Mat& image, bool root) -> bool {
  auto flat = gray_float(image);
  auto layout = infer_root_layout(state, image, flat);
  if (!layout) {
    return false;
  }
  // 根分类或分组入口直接比对高亮行号
  if (!group.entry || root) {
    int row = root_row_of(icon.definition->id);
    return row >= 0 && layout->highlighted == row;
  }
  // 子分类推算目标坐标并在视口内检测高亮边框信号
  auto area = bounds(group, *layout);
  auto point = target_point(group, icon, image, *layout, false);
  if (!area || !point || point->y - child_half_size < (*area)[1] ||
      point->y + child_half_size >= (*area)[3]) {
    return false;
  }
  return highlight_signal(flat, *point, true) >= selection_contrast;
}

// 根据目标距离与每格像素率换算滚轮格数
auto notches_for(int distance, double rate, int limit) -> int {
  return std::clamp(static_cast<int>(std::ceil(distance / rate)), 1, limit);
}

}  // namespace features::scan::menu

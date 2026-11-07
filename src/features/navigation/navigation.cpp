#include "features/navigation/navigation.hpp"

#include "vendor/std.hpp"

#include "core/i18n/generated/texts.hpp"
#include "core/message/message.hpp"
#include "core/state/app_state.hpp"
#include "features/capture/capture.hpp"
#include "features/navigation/state.hpp"
#include "features/scan/layout.hpp"
#include "features/scan/state.hpp"
#include "features/window/window.hpp"
#include "utils/image.hpp"
#include "utils/string.hpp"

namespace features::navigation {

namespace {

using Clock = std::chrono::steady_clock;
using Frame = capture::Frame;
constexpr scan::Box menu_rect{120, 57, 186, 485};
constexpr scan::Box root_search_rect{120, 63, 186, 469};
constexpr std::array<int, 2> outer_scroll_point{153, 84};
constexpr std::array<int, 2> park_point{193, 60};
constexpr double match_threshold = 0.8;
constexpr double match_margin = 0.05;
constexpr double selection_contrast = 3.0;
constexpr double signal_blur_sigma = 2.0;
constexpr int child_row_step = 45;
// 菜单滚轮像素率不硬编码：首次需要按像素率规划滚动时才用固定格数小步实测，整批次复用。
constexpr int calibration_notches = 2;
constexpr int calibration_min_matches = 2;
constexpr int calibration_shift_tolerance = 2;
constexpr double min_wheel_rate = 4.0;
constexpr double max_wheel_rate = 128.0;
constexpr int root_seek_notches = 6;
constexpr int child_seek_notches = 6;
constexpr int reveal_max_notches = 24;
constexpr int max_menu_scrolls = 30;
constexpr double hover_seconds = 0.15;
constexpr double click_seconds = 0.12;
constexpr auto input_check_interval = std::chrono::milliseconds(10);

constexpr std::array<int, 2> child_search_x{125, 181};
constexpr int child_top_offset = 29;
constexpr int child_min_height = 200;
constexpr int accessories_min_parent_gap = 267;
constexpr int next_parent_bottom_inset = 25;
constexpr int child_readable_inset = 25;
constexpr int child_target_top_offset = 53;
constexpr int root_half_size = 27;
constexpr int child_half_size = 21;
constexpr int selection_corner_inset = 8;
constexpr int selection_band_half = 5;
constexpr int selection_contrast_radius = 3;

// 左侧菜单外层行的固定排布：索引 0 是顶部无模板项，1..7 为服装分类，8/9 为分组入口。
// 数值为相对 hair 中心的累计像素偏移（960 分辨率实测）。滚动只改变原点，不改变间距。
constexpr std::array<int, 10> root_offsets{-55, 0, 55, 110, 165, 220, 276, 332, 391, 447};
constexpr std::array<std::string_view, 10> root_ids{
    "",        "hair",  "dresses", "outerwear",   "tops",
    "bottoms", "socks", "shoes",   "accessories", "makeup"};
constexpr int expanded_makeup_offset = 693;  // 饰品子视口展开后，妆容入口随之后移
constexpr int layout_anchor_tolerance = 2;
constexpr int layout_min_anchors = 2;
constexpr int layout_probe_radius = 3;              // 推算中心的取整误差补偿窗口
constexpr double layout_highlight_contrast = 20.0;  // 真高亮 44~49，杂峰不超过 16
constexpr double layout_highlight_ratio = 1.5;      // 唯一赢家须明显高于次高行
constexpr int child_origin_min_anchors = 2;

constexpr std::array<IconDefinition, 7> clothing_categories{{
    {"hair", 37},
    {"dresses", 37},
    {"outerwear", 37},
    {"tops", 37},
    {"bottoms", 37},
    {"socks", 37},
    {"shoes", 37},
}};

constexpr std::array<IconDefinition, 15> accessories_categories{{
    {"hair_accessories", 29},
    {"headwear", 29},
    {"earrings", 29},
    {"neckwear", 29},
    {"bracelets", 29},
    {"chokers", 29},
    {"gloves", 32},
    {"face_decorations", 29},
    {"chest_accessories", 29},
    {"pendants", 29},
    {"backpieces", 29},
    {"rings", 29},
    {"arm_decorations", 29},
    {"handhelds", 29},
    {"body_paint", 29},
}};

constexpr std::array<IconDefinition, 5> makeup_categories{{
    {"base_makeup", 29},
    {"eyebrows", 29},
    {"eyelashes", 29},
    {"contact_lenses", 29},
    {"lips", 29},
}};

constexpr std::array<GroupDefinition, 3> groups{{
    {.id = "clothing", .categories = clothing_categories},
    {.id = "accessories",
     .expanded_parent_y = 148,
     .entry = IconDefinition{"accessories", 37},
     .categories = accessories_categories},
    {.id = "makeup",
     .expanded_parent_y = 204,
     .viewport_bottom_offset = 279,
     .bottom_fade_inset = 45,
     .entry = IconDefinition{"makeup", 37},
     .categories = makeup_categories},
}};

// 动态拼接的分类与分组翻译键也必须存在于生成文本表中；不另维护第二份分类清单。
constexpr auto has_label(std::string_view prefix, std::string_view id) -> bool {
  for (const auto& text : core::i18n::generated::texts) {
    if (text.key.starts_with(prefix) && text.key.substr(prefix.size()) == id) {
      return true;
    }
  }
  return false;
}

static_assert([] {
  for (const auto& group : groups) {
    if (!has_label("group.", group.id) || group.categories.empty() || group.bottom_fade_inset < 0 ||
        group.bottom_fade_inset > child_row_step) {
      return false;
    }
    if (group.entry && (group.entry->id != group.id || group.entry->size <= 0)) {
      return false;
    }
    for (const auto& icon : group.categories) {
      if (icon.id.empty() ||
          icon.id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") !=
              std::string_view::npos ||
          icon.size <= 0 || !has_label("category.", icon.id)) {
        return false;
      }
      int count = 0;
      for (const auto& other_group : groups) {
        for (const auto& other : other_group.categories) {
          count += other.id == icon.id;
        }
      }
      if (count != 1) {
        return false;
      }
    }
  }
  return true;
}());
static_assert(groups[0].id == "clothing" && !groups[0].entry && groups[0].categories.size() >= 3 &&
              groups[1].id == "accessories" && groups[1].entry && groups[2].id == "makeup" &&
              groups[2].entry);
static_assert(calibration_notches > 0 && calibration_notches <= window::max_wheel_notches &&
              root_seek_notches > 0 && root_seek_notches <= window::max_wheel_notches &&
              child_seek_notches > 0 && child_seek_notches <= window::max_wheel_notches &&
              reveal_max_notches > 0 && reveal_max_notches <= window::max_wheel_notches);
static_assert(min_wheel_rate > 0 && max_wheel_rate > min_wheel_rate);
static_assert([] {
  if (root_offsets.size() != root_ids.size() || root_offsets[1] != 0 ||
      expanded_makeup_offset <= root_offsets.back() || !root_ids.front().empty()) {
    return false;
  }
  for (std::size_t i = 1; i < root_offsets.size(); ++i) {
    if (root_offsets[i] <= root_offsets[i - 1]) {
      return false;
    }
  }
  int entries = 0;
  for (std::size_t i = 0; i < root_ids.size(); ++i) {
    for (std::size_t j = i + 1; j < root_ids.size(); ++j) {
      if (root_ids[i] == root_ids[j]) {
        return false;
      }
    }
    if (!root_ids[i].empty()) {
      bool found = false;
      for (const auto& group : groups) {
        found |= group.entry && group.entry->id == root_ids[i];
        for (const auto& icon : group.categories) {
          found |= icon.id == root_ids[i];
        }
      }
      if (!found) {
        return false;
      }
      ++entries;
    }
  }
  return entries == static_cast<int>(root_ids.size()) - 1;
}());

// 将 BGR 图像转换为灰度图像
auto gray(const cv::Mat& image) -> cv::Mat {
  cv::Mat result;
  cv::cvtColor(image, result, cv::COLOR_BGR2GRAY);
  return result;
}

// 预计算整帧灰度浮点图：逐行量高亮时复用，避免对大图反复做格式转换。
auto gray_float(const cv::Mat& pixels) -> cv::Mat {
  cv::Mat result;
  gray(pixels).convertTo(result, CV_32F);
  return result;
}

// 提取导航图标信号特征（灰度化并高斯模糊降噪）
auto icon_signal(const cv::Mat& image) -> cv::Mat {
  cv::Mat value;
  cv::Mat blurred;
  cv::Mat result;
  // 转灰度浮点图并执行高斯低通滤波
  gray(image).convertTo(value, CV_32F);
  cv::GaussianBlur(value, blurred, {0, 0}, signal_blur_sigma);
  // 通过原图减去平滑图提取高频边缘特征并截断负值
  cv::subtract(value, blurred, result);
  cv::max(result, 0, result);
  return result;
}

// 模板匹配定位导航图标：在指定搜索矩形内执行灰度相关性匹配并筛选最佳峰值
auto locate(const cv::Mat& image, const Icon& icon, const scan::Box& bounds, bool child = false)
    -> std::optional<Match> {
  // 提取搜索区域的高频边缘特征图
  auto query = icon_signal(image(scan::rect(bounds)));
  int width = icon.signal.cols;
  int height = icon.signal.rows;
  if (query.cols < width || query.rows < height) {
    return {};
  }
  // 在搜索区内进行归一化相关性模板匹配
  cv::Mat response;
  cv::matchTemplate(query, icon.signal, response, cv::TM_CCOEFF_NORMED);
  // 无纹理位置的匹配值可能不是有限数，排除后再寻找有效峰值。
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
  // 消除最佳峰值邻域并寻找次优峰，计算峰值尖锐度 (margin)
  int left = std::max(0, best.x - width / 2);
  int top = std::max(0, best.y - height / 2);
  int right = std::min(response.cols, best.x + width / 2 + 1);
  int bottom = std::min(response.rows, best.y + height / 2 + 1);
  response(cv::Rect(left, top, right - left, bottom - top)).setTo(-1);
  double second{};
  cv::minMaxLoc(response, nullptr, &second);
  // 换算图标在客户区中的绝对点击目标点
  cv::Point point = cv::Point(bounds[0], bounds[1]) + best +
                    cv::Point(icon.definition->size / 2, icon.definition->size / 2);
  const auto& vertical = child ? bounds : menu_rect;
  int half = child ? child_half_size : root_half_size;
  // 校验匹配置信度、峰值尖锐度及纵向有效边界
  if (score < match_threshold || score - second < match_margin || point.y - half < vertical[1] ||
      point.y + half >= vertical[3]) {
    return {};
  }
  return Match{point};
}

// 在指定时间内暂停并持续检查窗口有效性与停止请求
auto pause(core::AppState& state, double duration) -> std::expected<void, core::Error> {
  auto deadline = Clock::now() + std::chrono::duration<double>(duration);
  // 细粒度轮询等待，及时响应停止请求和游戏失焦。
  while (Clock::now() < deadline) {
    if (auto ok = window::check(state, state.scan->resources.target); !ok) {
      return ok;
    }
    std::this_thread::sleep_for(input_check_interval);
  }
  return {};
}

// 将鼠标指针移动到停放泊位坐标，避免悬停触发高亮动效
auto park(core::AppState& state) -> std::expected<void, core::Error> {
  auto& nav = *state.navigation;
  auto point = park_point;
  // 移动光标至预设的安全无交互空白区域
  if (auto ok = window::position_pointer(state, state.scan->resources.target, {point[0], point[1]});
      !ok) {
    return ok;
  }
  nav.pointer_parked = true;
  return {};
}

// 发送输入并返回完成边界；点击后停放以消除悬停，滚轮保留安全输入点。
auto send_input(core::AppState& state, cv::Point point, int notches = 0)
    -> std::expected<std::int64_t, core::Error> {
  auto& nav = *state.navigation;
  // 先把光标移动到目标交互点
  if (auto ok = window::position_pointer(state, state.scan->resources.target, {point.x, point.y});
      !ok) {
    return std::unexpected(ok.error());
  }
  nav.pointer_parked = false;
  // 等待悬停稳定
  if (auto ok = pause(state, hover_seconds); !ok) {
    return std::unexpected(ok.error());
  }
  // 发送物理点击或滚轮事件
  auto sent = notches ? window::wheel(state, state.scan->resources.target, notches)
                      : window::click(state, state.scan->resources.target);
  if (!sent) {
    return std::unexpected(sent.error());
  }
  // 点击后消除悬停样式，再记录输入完成边界。
  if (!notches) {
    if (auto ok = pause(state, click_seconds); !ok) {
      return std::unexpected(ok.error());
    }
    if (auto ok = park(state); !ok) {
      return std::unexpected(ok.error());
    }
  }
  return capture::source_time();
}

// GPU 完成统一判稳后才检查导航目标；未就绪时等画面变化，不重发输入。
// 超时不产生底层错误，失败文案由调用方按所处阶段给出。
auto wait_navigation(core::AppState& state, std::int64_t after_source_time,
                     const std::function<bool(const cv::Mat&)>& predicate = {},
                     std::string_view timeout_key = "error.navigation.timeout")
    -> std::expected<Frame, core::Error> {
  auto& nav = *state.navigation;
  auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                     std::chrono::duration<double>(nav.timeout));
  bool require_change = false;
  while (Clock::now() < deadline) {
    auto frame = capture::wait_stable_frame(
        state, after_source_time, deadline,
        [&] { return window::check(state, state.scan->resources.target); }, require_change);
    if (!frame) {
      auto error = std::move(frame.error());
      error.related.push_back({nav.stage, {}, {}});
      return std::unexpected(std::move(error));
    }
    if (!*frame) {
      return std::unexpected(core::Error{core::message(timeout_key), {}, {{nav.stage, {}, {}}}});
    }
    nav.last_frame = *frame;
    if (predicate && !predicate((**frame).bgr)) {
      after_source_time = (**frame).source_time;
      require_change = true;
      continue;
    }
    return std::move(**frame);
  }
  return std::unexpected(core::Error{core::message(timeout_key), {}, {{nav.stage, {}, {}}}});
}

// 计算图标按钮选中指示高光与周围背景的亮度对比度；image 为整帧 CV_32F 灰度图。
auto contrast(const cv::Mat& image, cv::Point point, bool child) -> double {
  int half = child ? child_half_size : root_half_size;
  int span = half - selection_corner_inset;
  // 分析四个边缘采样带的高光峰值
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

// 推算中心与真实按钮中心可能有几像素取整误差，在小窗口内取最强亮框响应。
// 采样窗口只受整帧菜单边界约束：子项「可读区」是滚动规划的安全边距，不参与亮框判定。
auto highlight_signal(const cv::Mat& flat, cv::Point point, bool child) -> double {
  int half = child ? child_half_size : root_half_size;
  int top = menu_rect[1] + half + selection_band_half;
  int bottom = menu_rect[3] - half - selection_band_half;
  double best = -std::numeric_limits<double>::infinity();
  for (int dy = -layout_probe_radius; dy <= layout_probe_radius; ++dy) {
    int y = point.y + dy;
    if (y >= top && y < bottom) {
      best = std::max(best, contrast(flat, cv::Point{point.x, y}, child));
    }
  }
  return best;
}

// 当前帧推断出的整列排布：origin 为 hair 行中心；高亮以「第几行」表达。
struct RootLayout {
  int origin{};
  bool expanded{};      // 饰品子视口展开，妆容入口整体下移
  int highlighted{-1};  // 当前高亮所在行；-1 表示没有可靠高亮
  std::array<cv::Point, root_offsets.size()> points{};
};

// 分类与分组入口共用同一套 id，按 id 取运行时图标资源。
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

auto root_row_of(std::string_view id) -> int {
  for (int row = 0; row < static_cast<int>(root_ids.size()); ++row) {
    if (root_ids[row] == id) {
      return row;
    }
  }
  return -1;
}

auto root_offset(int row, bool expanded) -> int {
  return expanded && row == static_cast<int>(root_offsets.size()) - 1 ? expanded_makeup_offset
                                                                      : root_offsets[row];
}

// 用所有可识别的根图标投票拟合菜单原点，再逐行量高亮，得出「当前亮的是第几项」。
// 收起与饰品展开两套排布分别拟合，取锚点最一致的一套：点击引起的滚动、子类别收起
// 都只会改变原点，不会改变行距，因此每帧重新拟合即可，无需认出目标图标本身。
auto infer_root_layout(const core::AppState& state, const cv::Mat& image, const cv::Mat& flat)
    -> std::optional<RootLayout> {
  const auto& catalog = state.navigation->catalog;
  std::vector<std::pair<int, int>> anchors;
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
  int best_count{};
  int best_origin{};
  bool best_expanded{};
  for (bool expanded : {false, true}) {
    std::vector<int> origins;
    for (const auto& [row, y] : anchors) {
      origins.push_back(y - root_offset(row, expanded));
    }
    std::ranges::sort(origins);
    int median = origins[origins.size() / 2];
    int count = static_cast<int>(std::ranges::count_if(origins, [median](int value) {
      return std::abs(value - median) <= layout_anchor_tolerance;
    }));
    if (count > best_count) {
      best_count = count;
      best_origin = median;
      best_expanded = expanded;
    }
  }
  if (best_count < layout_min_anchors) {
    return {};
  }
  RootLayout layout{.origin = best_origin, .expanded = best_expanded};
  for (int row = 0; row < static_cast<int>(root_offsets.size()); ++row) {
    layout.points[row] =
        cv::Point{outer_scroll_point[0], best_origin + root_offset(row, best_expanded)};
  }
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
  // 没有明显赢家时宁可判「未知」，避免把图标自身边缘误当成选中高亮。
  if (layout.highlighted < 0 || best < second * layout_highlight_ratio) {
    layout.highlighted = -1;
  }
  return layout;
}

// 动态计算子项视口的可见裁剪搜索框；父入口位置由排布推算，不再依赖图标识别。
auto bounds(const Group& group, const RootLayout& layout) -> std::optional<scan::Box> {
  if (!group.entry) {
    return root_search_rect;
  }
  int row = root_row_of(group.entry->definition->id);
  if (row < 0) {
    return {};
  }
  int parent_y = layout.points[row].y;
  int bottom;
  // 饰品分组通过妆容入口的下移距离判断子视口是否已经展开
  if (group.definition->id == "accessories") {
    int next_y = layout.points[static_cast<int>(root_offsets.size()) - 1].y;
    if (next_y - parent_y < accessories_min_parent_gap) {
      return {};
    }
    bottom = next_y - next_parent_bottom_inset;
  } else {
    bottom = parent_y + group.definition->viewport_bottom_offset;
    if (bottom > menu_rect[3]) {
      return {};
    }
  }
  // 物理展开区仍可能包含底部渐隐。定位、漏识别保护和滚动规划共用可读边界。
  bottom -= group.definition->bottom_fade_inset;
  if (bottom - (parent_y + child_top_offset) < child_min_height) {
    return {};
  }
  return scan::Box{child_search_x[0], parent_y + child_top_offset, child_search_x[1], bottom};
}

auto category_index(const Group& group, const Icon& icon) -> int {
  auto found = std::ranges::find_if(group.categories, [&](const Icon& item) {
    return item.definition->id == icon.definition->id;
  });
  return static_cast<int>(found - group.categories.begin());
}

// 子类别是独立滚动的视口：用同组所有可识别子项投票得到列表原点，再按行距推算任意一行。
auto child_origin(const Group& group, const cv::Mat& image, const scan::Box& area)
    -> std::optional<int> {
  std::vector<int> origins;
  for (int i = 0; i < static_cast<int>(group.categories.size()); ++i) {
    if (auto match = locate(image, group.categories[i], area, true)) {
      origins.push_back(match->point.y - i * child_row_step);
    }
  }
  if (origins.size() < child_origin_min_anchors) {
    return {};
  }
  std::ranges::sort(origins);
  int median = origins[origins.size() / 2];
  int consistent = static_cast<int>(std::ranges::count_if(origins, [median](int value) {
    return std::abs(value - median) <= layout_anchor_tolerance;
  }));
  if (consistent < child_origin_min_anchors) {
    return {};
  }
  return median;
}

// 目标在当前布局中的位置：根行直接查排布表，子项按独立视口原点与行距推算。
auto target_point(const Group& group, const Icon& icon, const cv::Mat& image,
                  const RootLayout& layout, bool root) -> std::optional<cv::Point> {
  if (!group.entry || root) {
    int row = root_row_of(icon.definition->id);
    if (row < 0) {
      return {};
    }
    return layout.points[row];
  }
  auto area = bounds(group, layout);
  if (!area) {
    return {};
  }
  auto origin = child_origin(group, image, *area);
  if (!origin) {
    return {};
  }
  int index = category_index(group, icon);
  if (index >= static_cast<int>(group.categories.size())) {
    return {};
  }
  return cv::Point{outer_scroll_point[0], *origin + index * child_row_step};
}

// 判定当前高亮是否落在目标分类上：先推断整列排布、读出高亮行，不再要求认出目标图标。
auto selected(const core::AppState& state, const Group& group, const Icon& icon,
              const cv::Mat& image, bool root = false) -> bool {
  auto flat = gray_float(image);
  auto layout = infer_root_layout(state, image, flat);
  if (!layout) {
    return false;
  }
  if (!group.entry || root) {
    int row = root_row_of(icon.definition->id);
    return row >= 0 && layout->highlighted == row;
  }
  auto area = bounds(group, *layout);
  auto point = target_point(group, icon, image, *layout, false);
  // 亮框判定只要求目标在视口里物理可见；可读边距决定「要不要再滚一点」，不否决判定结果。
  if (!area || !point || point->y - child_half_size < (*area)[1] ||
      point->y + child_half_size >= (*area)[3]) {
    return false;
  }
  return highlight_signal(flat, *point, true) >= selection_contrast;
}

// 计算滚动目标距离所需的滚轮格数
auto notches_for(int distance, double rate, int limit) -> int {
  return std::clamp(static_cast<int>(std::ceil(distance / rate)), 1, limit);
}

// 用固定格数小步滚动菜单，实测同一批图标的纵向位移，得出每格像素率后原路滚回。
// 只依赖图标定位，不依赖分辨率常量；与物品列表的自动标定互相独立。
// point 取调用方即将使用的滚动输入点，使探测与后续滚动落在同一区域。
auto calibrate_wheel(core::AppState& state, cv::Point point) -> std::expected<double, core::Error> {
  auto& nav = *state.navigation;
  using Positions = std::vector<std::pair<const Icon*, int>>;
  auto positions_of = [](const cv::Mat& image, const Positions& reference) -> Positions {
    Positions result;
    for (const auto& item : reference) {
      if (auto match = locate(image, *item.first, root_search_rect)) {
        result.emplace_back(item.first, match->point.y);
      }
    }
    return result;
  };
  // 取同一批图标前后纵坐标之差的中位数，要求多枚一致，排除个别误匹配。
  auto median_shift = [](const Positions& from, const Positions& to) -> int {
    std::vector<int> shifts;
    for (const auto& item : from) {
      const Icon* icon = item.first;
      auto found =
          std::ranges::find_if(to, [icon](const auto& other) { return other.first == icon; });
      if (found != to.end()) {
        shifts.push_back(item.second - found->second);
      }
    }
    std::ranges::sort(shifts);
    if (shifts.empty()) {
      return 0;
    }
    int median = shifts[shifts.size() / 2];
    int consistent = static_cast<int>(std::ranges::count_if(shifts, [median](int value) {
      return std::abs(value - median) <= calibration_shift_tolerance;
    }));
    return consistent >= calibration_min_matches ? median : 0;
  };

  nav.stage = core::message("navigation.stage.calibrating");
  // 首次触发可能发生在分组展开之后，此时常规根图标只剩一枚。
  // 因此把全部分类与分组入口都作为候选参照，统一在根搜索框内定位，中位数会过滤个别误匹配。
  Positions before;
  for (const auto& group : nav.catalog.groups) {
    if (group.entry) {
      if (auto match = locate(nav.last_frame->bgr, *group.entry, root_search_rect)) {
        before.emplace_back(&*group.entry, match->point.y);
      }
    }
    for (const auto& icon : group.categories) {
      if (auto match = locate(nav.last_frame->bgr, icon, root_search_rect)) {
        before.emplace_back(&icon, match->point.y);
      }
    }
  }
  if (before.size() < calibration_min_matches) {
    return std::unexpected(core::error("error.navigation.calibration_failed"));
  }
  auto down = send_input(state, point, calibration_notches);
  if (!down) {
    return std::unexpected(down.error());
  }
  if (auto moved = wait_navigation(state, *down); !moved) {
    return std::unexpected(moved.error());
  }
  // 向下滚动使图标上移，位移为正。
  Positions after_down = positions_of(nav.last_frame->bgr, before);
  int down_shift = median_shift(before, after_down);
  // 无论标定是否成功都原路滚回，避免把菜单留在非初始位置。
  auto up = send_input(state, point, -calibration_notches);
  if (!up) {
    return std::unexpected(up.error());
  }
  auto restored = wait_navigation(
      state, *up,
      [&before](const cv::Mat& image) {
        int matched = 0;
        for (const auto& [icon, y] : before) {
          if (auto match = locate(image, *icon, root_search_rect);
              match && std::abs(match->point.y - y) <= calibration_shift_tolerance) {
            ++matched;
          }
        }
        return matched >= calibration_min_matches;
      },
      "error.navigation.calibration_failed");
  if (!restored) {
    return std::unexpected(restored.error());
  }
  if (auto parked = park(state); !parked) {
    return std::unexpected(parked.error());
  }
  // 回程位移应与下行一致；不一致说明滚到了边界被截断，此时速率不可信。
  int up_shift = -median_shift(after_down, positions_of(nav.last_frame->bgr, after_down));
  double rate = static_cast<double>(down_shift) / calibration_notches;
  if (down_shift <= 0 || std::abs(down_shift - up_shift) > calibration_shift_tolerance ||
      rate < min_wheel_rate || rate > max_wheel_rate) {
    return std::unexpected(core::error("error.navigation.calibration_failed"));
  }
  return rate;
}

// 惰性标定：只有首次确实要按像素率换算滚轮格数时才探测，结果整批次复用。
// 返回 true 表示本次刚完成探测（菜单已原路回位），调用方须基于新帧重新规划滚动。
auto ensure_wheel_rate(core::AppState& state, cv::Point point) -> std::expected<bool, core::Error> {
  auto& nav = *state.navigation;
  if (nav.pixels_per_notch > 0) {
    return false;
  }
  auto rate = calibrate_wheel(state, point);
  if (!rate) {
    return std::unexpected(rate.error());
  }
  nav.pixels_per_notch = *rate;
  return true;
}

// 滚动寻找视野外的分类或分组图标：循环滚动微调 → 等待画面静止 → 匹配定位图标中心
auto seek(core::AppState& state, const Group& group, const Icon& icon, int direction = 1,
          bool root = false) -> std::expected<Match, core::Error> {
  auto& nav = *state.navigation;
  bool child = group.entry.has_value() && !root;
  nav.stage = core::message(
      "navigation.stage.seeking", core::text_key(std::format("group.{}", group.definition->id)),
      core::text_key(std::format("{}.{}", root ? "group" : "category", icon.definition->id)));
  // 限制最大滚轮尝试次数，防止无限循环；标定只重规划帧，不占用尝试次数。
  for (int count = 0; count <= max_menu_scrolls;) {
    if (auto ok = window::check(state, state.scan->resources.target); !ok) {
      return std::unexpected(ok.error());
    }
    const auto& image = nav.last_frame->bgr;
    auto flat = gray_float(image);
    // 每次点击后的稳定帧都重新拟合原点，滚动与子类别收起不会让推算坐标失效。
    auto layout = infer_root_layout(state, image, flat);
    if (!layout) {
      return std::unexpected(
          core::error("error.navigation.bounds_unknown",
                      core::text_key(std::format("group.{}", group.definition->id))));
    }
    auto area = child ? bounds(group, *layout) : std::optional{root_search_rect};
    if (!area) {
      return std::unexpected(
          core::error("error.navigation.bounds_unknown",
                      core::text_key(std::format("group.{}", group.definition->id))));
    }
    // 常态图标能直接认出时用精确中心
    if (auto match = locate(image, icon, *area, child)) {
      return *match;
    }
    // 目标已选中时图标外观会变化、匹配不上，改由整列排布推算它的位置。
    std::optional<cv::Point> expected = target_point(group, icon, image, *layout, root);
    if (expected) {
      int top = child ? (*area)[1] : menu_rect[1];
      int bottom = child ? (*area)[3] : menu_rect[3];
      int half = child ? child_half_size : root_half_size;
      bool visible = expected->y - half >= top && expected->y + half < bottom;
      if (child) {
        visible = visible && expected->y >= (*area)[1] + child_readable_inset &&
                  expected->y < (*area)[3] - child_readable_inset;
      }
      if (visible) {
        return Match{*expected};
      }
    }
    if (count == max_menu_scrolls) {
      break;
    }
    // 规划本次滚轮输入点与格数
    cv::Point point{outer_scroll_point[0], outer_scroll_point[1]};
    int notches = root_seek_notches;
    if (child) {
      point = {outer_scroll_point[0], ((*area)[1] + (*area)[3]) / 2};
      auto calibrated = ensure_wheel_rate(state, point);
      if (!calibrated) {
        return std::unexpected(calibrated.error());
      }
      if (*calibrated) {
        continue;  // 探测后菜单已回位，重新基于最新帧定位与推算
      }
      double rate = nav.pixels_per_notch;
      int limit = std::max(1, static_cast<int>(((*area)[3] - (*area)[1] - child_row_step) / rate));
      int distance = expected ? direction * (expected->y - ((*area)[1] + child_target_top_offset))
                              : static_cast<int>(child_seek_notches * rate);
      if (distance <= 0) {
        return std::unexpected(
            core::error("error.navigation.wrong_direction",
                        core::text_key(std::format("category.{}", icon.definition->id))));
      }
      notches = notches_for(distance, rate, limit);
    }
    // 注入滚轮输入并等待画面静止
    auto sent = send_input(state, point, direction * notches);
    if (!sent) {
      return std::unexpected(sent.error());
    }
    ++count;
    auto frame = wait_navigation(state, *sent);
    if (!frame) {
      return std::unexpected(frame.error());
    }
  }
  return std::unexpected(core::error(
      "error.navigation.seek_limit",
      core::text_key(std::format("{}.{}", root ? "group" : "category", icon.definition->id))));
}

}  // namespace

// 分类定义为只读静态数据；这里只创建本次批次的图像资源。
auto load(const std::filesystem::path& directory) -> std::expected<Catalog, core::Error> {
  auto make_icon = [&](const IconDefinition& definition) -> std::expected<Icon, core::Error> {
    auto pixels =
        utils::image::load(directory / utils::string::wide(std::format("{}.png", definition.id)));
    if (!pixels) {
      return std::unexpected(pixels.error());
    }
    if (pixels->cols != definition.size || pixels->rows != definition.size) {
      return std::unexpected(core::error("error.navigation.invalid_icon_geometry", definition.id));
    }
    auto signal = icon_signal(*pixels);
    if (cv::norm(signal) == 0) {
      return std::unexpected(core::error("error.navigation.empty_icon", definition.id));
    }
    return Icon{&definition, std::move(signal)};
  };
  Catalog catalog;
  for (const auto& definition : groups) {
    Group runtime{.definition = &definition};
    if (definition.entry) {
      auto icon = make_icon(*definition.entry);
      if (!icon) {
        return std::unexpected(icon.error());
      }
      runtime.entry = std::move(*icon);
    }
    for (const auto& category : definition.categories) {
      auto icon = make_icon(category);
      if (!icon) {
        return std::unexpected(icon.error());
      }
      runtime.categories.push_back(std::move(*icon));
    }
    catalog.groups.push_back(std::move(runtime));
  }
  return catalog;
}

// 初始化导航会话：停靠鼠标 → 确认左侧菜单就绪。
// 滚轮像素率留到首次需要按像素率滚动时才实测，启动阶段不发任何滚轮输入。
auto begin(core::AppState& state) -> std::expected<void, core::Error> {
  auto& nav = *state.navigation;
  // 先将鼠标移到停泊区，避免干扰初始界面识别
  if (auto ok = park(state); !ok) {
    return ok;
  }
  const auto& regular = nav.catalog.groups[0].categories;
  nav.stage = core::message("navigation.stage.waiting_initial");
  // 等待首屏前三个常规分类全部就绪出现；启动阶段等不到菜单即判定界面未就绪。
  auto frame = wait_navigation(
      state, capture::source_time(),
      [&](const cv::Mat& image) {
        for (int i = 0; i < 3; ++i) {
          if (!locate(image, regular[i], root_search_rect)) {
            return false;
          }
        }
        return true;
      },
      "error.navigation.menu_not_found");
  if (!frame) {
    return std::unexpected(frame.error());
  }
  return {};
}

// 展开指定分组条目：检查是否已展开 → 若折叠则定位入口并点击展开
auto open_group(core::AppState& state, const Group& group) -> std::expected<void, core::Error> {
  auto& nav = *state.navigation;
  // 无父级入口的根组（如服装）直接跳过展开阶段
  if (!group.entry) {
    return {};
  }
  // 滚动搜索定位分组父级图标
  auto match = seek(state, group, *group.entry, 1, true);
  if (!match) {
    return std::unexpected(match.error());
  }
  nav.stage = core::message("navigation.stage.opening",
                            core::text_key(std::format("group.{}", group.definition->id)));
  // 点击展开分组
  auto sent = send_input(state, match->point);
  if (!sent) {
    return std::unexpected(sent.error());
  }
  // 等待展开并确认父级处于选中状态
  auto frame = wait_navigation(state, *sent, [&](const cv::Mat& image) {
    return selected(state, group, *group.entry, image, true);
  });
  if (!frame) {
    return std::unexpected(frame.error());
  }
  // 若子分类视口尚未完整露出来，向下滚动微调视口；标定只重规划帧，不占用尝试次数。
  // 父入口坐标改用排布推算：展开后它处于选中态，图标本身可能已经认不出。
  std::optional<RootLayout> layout;
  for (int count = 0;;) {
    const auto& image = nav.last_frame->bgr;
    auto flat = gray_float(image);
    layout = infer_root_layout(state, image, flat);
    if (!layout) {
      return std::unexpected(
          core::error("error.navigation.parent_lost",
                      core::text_key(std::format("group.{}", group.definition->id))));
    }
    if (bounds(group, *layout)) {
      break;
    }
    if (count == max_menu_scrolls) {
      return std::unexpected(core::error("error.navigation.reveal_limit"));
    }
    int row = root_row_of(group.entry->definition->id);
    if (row < 0) {
      return std::unexpected(
          core::error("error.navigation.parent_lost",
                      core::text_key(std::format("group.{}", group.definition->id))));
    }
    int distance = layout->points[row].y - group.definition->expanded_parent_y;
    if (distance <= 0) {
      return std::unexpected(core::error("error.navigation.child_bounds_unknown"));
    }
    nav.stage = core::message("navigation.stage.revealing",
                              core::text_key(std::format("group.{}", group.definition->id)));
    const cv::Point point{outer_scroll_point[0], outer_scroll_point[1]};
    auto calibrated = ensure_wheel_rate(state, point);
    if (!calibrated) {
      return std::unexpected(calibrated.error());
    }
    if (*calibrated) {
      continue;  // 探测后菜单已回位，重新拟合原点与视口
    }
    double rate = nav.pixels_per_notch;
    int notches = notches_for(distance, rate, reveal_max_notches);
    auto boundary = send_input(state, point, notches);
    if (!boundary) {
      return std::unexpected(boundary.error());
    }
    ++count;
    auto moved = wait_navigation(state, *boundary);
    if (!moved) {
      return std::unexpected(moved.error());
    }
  }
  auto area = *bounds(group, *layout);
  // 确保展开区域内至少能识别到任一子分类图标
  if (!std::ranges::any_of(group.categories, [&](const Icon& icon) {
        return locate(nav.last_frame->bgr, icon, area, true).has_value();
      })) {
    return std::unexpected(core::error("error.navigation.no_child_icons"));
  }
  // 将首个目标分类滚到可读区，不在定位阶段点击。
  auto first = seek(state, group, group.categories.front(), -1);
  if (!first) {
    return std::unexpected(first.error());
  }
  return {};
}

// 导航切换到指定分类：定位目标图标 → 校验选中状态 → 模拟点击切换并等待列表就绪
auto visit(core::AppState& state, const Group& group, const Icon& icon)
    -> std::expected<void, core::Error> {
  auto& nav = *state.navigation;
  // 滚动并定位目标分类图标位置
  auto match = seek(state, group, icon);
  if (!match) {
    return std::unexpected(match.error());
  }
  nav.stage = core::message("navigation.stage.selecting",
                            core::text_key(std::format("group.{}", group.definition->id)),
                            core::text_key(std::format("category.{}", icon.definition->id)));
  // 确保鼠标已停靠，避免悬停伪高光干扰
  if (!nav.pointer_parked) {
    if (auto parked = park(state); !parked) {
      return parked;
    }
    // 悬停可能影响选中外观；先确认停放后的新帧稳定，不以目标已选中作为等待条件。
    auto frame = wait_navigation(state, capture::source_time());
    if (!frame) {
      return std::unexpected(frame.error());
    }
    auto flat = gray_float(nav.last_frame->bgr);
    auto layout = infer_root_layout(state, nav.last_frame->bgr, flat);
    std::optional<cv::Point> refreshed;
    if (layout) {
      refreshed = target_point(group, icon, nav.last_frame->bgr, *layout, false);
    }
    if (!refreshed) {
      return std::unexpected(core::error("error.navigation.target_lost_after_park"));
    }
    match->point = *refreshed;
  }
  // 若目标尚未处于选中高亮状态，发送点击并等待状态切换生效
  if (!selected(state, group, icon, nav.last_frame->bgr)) {
    auto sent = send_input(state, match->point);
    if (!sent) {
      return std::unexpected(sent.error());
    }
    auto frame = wait_navigation(
        state, *sent, [&](const cv::Mat& image) { return selected(state, group, icon, image); });
    if (!frame) {
      return std::unexpected(frame.error());
    }
  }
  // 返回前再确认目标确实亮着；已选中的图标无需（也往往无法）重新识别。
  if (!selected(state, group, icon, nav.last_frame->bgr)) {
    return std::unexpected(core::error("error.navigation.target_lost_after_selection"));
  }
  return {};
}

}  // namespace features::navigation

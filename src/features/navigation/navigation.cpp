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
constexpr scan::Box menu_rect{180, 85, 279, 728};
constexpr scan::Box root_search_rect{180, 95, 279, 704};
constexpr std::array<int, 2> outer_scroll_point{229, 126};
constexpr std::array<int, 2> park_point{290, 90};
constexpr double match_threshold = 0.8;
constexpr double match_margin = 0.05;
constexpr double selection_contrast = 3.0;
constexpr double signal_blur_sigma = 3.0;
constexpr int child_row_step = 68;
constexpr int wheel_pixels_per_notch = 24;
constexpr int root_seek_notches = 6;
constexpr int child_seek_notches = 6;
constexpr int reveal_max_notches = 24;
constexpr int max_menu_scrolls = 30;
constexpr double hover_seconds = 0.15;
constexpr double click_seconds = 0.12;
constexpr auto input_check_interval = std::chrono::milliseconds(10);

constexpr std::array<int, 2> child_search_x{188, 272};
constexpr int child_top_offset = 43;
constexpr int child_min_height = 300;
constexpr int accessories_min_parent_gap = 400;
constexpr int next_parent_bottom_inset = 38;
constexpr int child_readable_inset = 37;
constexpr int child_target_top_offset = 80;
constexpr int root_half_size = 40;
constexpr int child_half_size = 32;
constexpr int selection_corner_inset = 12;
constexpr int selection_band_half = 8;
constexpr int selection_contrast_radius = 4;

constexpr std::array<IconDefinition, 7> clothing_categories{{
    {"hair", 56},
    {"dresses", 56},
    {"outerwear", 56},
    {"tops", 56},
    {"bottoms", 56},
    {"socks", 56},
    {"shoes", 56},
}};

constexpr std::array<IconDefinition, 15> accessories_categories{{
    {"hair_accessories", 44},
    {"headwear", 44},
    {"earrings", 44},
    {"neckwear", 44},
    {"bracelets", 44},
    {"chokers", 44},
    {"gloves", 48},
    {"face_decorations", 44},
    {"chest_accessories", 44},
    {"pendants", 44},
    {"backpieces", 44},
    {"rings", 44},
    {"arm_decorations", 44},
    {"handhelds", 44},
    {"body_paint", 44},
}};

constexpr std::array<IconDefinition, 5> makeup_categories{{
    {"base_makeup", 44},
    {"eyebrows", 44},
    {"eyelashes", 44},
    {"contact_lenses", 44},
    {"lips", 44},
}};

constexpr std::array<GroupDefinition, 3> groups{{
    {.id = "clothing", .categories = clothing_categories},
    {.id = "accessories",
     .expanded_parent_y = 222,
     .entry = IconDefinition{"accessories", 56},
     .categories = accessories_categories},
    {.id = "makeup",
     .expanded_parent_y = 306,
     .viewport_bottom_offset = 418,
     .bottom_fade_inset = 68,
     .entry = IconDefinition{"makeup", 56},
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
static_assert(root_seek_notches > 0 && root_seek_notches <= window::max_wheel_notches &&
              child_seek_notches > 0 && child_seek_notches <= window::max_wheel_notches &&
              reveal_max_notches > 0 && reveal_max_notches <= window::max_wheel_notches);

// 将 BGR 图像转换为灰度图像
auto gray(const cv::Mat& image) -> cv::Mat {
  cv::Mat result;
  cv::cvtColor(image, result, cv::COLOR_BGR2GRAY);
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

// 动态计算分类组子项在视口中的可见裁剪搜索框
auto bounds(const core::AppState& state, const Group& group, const cv::Mat& image)
    -> std::optional<scan::Box> {
  const auto& nav = *state.navigation;
  // 顶层根分类直接使用全局搜索矩形
  if (!group.entry) {
    return root_search_rect;
  }
  // 先定位父级分组入口图标
  auto parent = locate(image, *group.entry, root_search_rect);
  if (!parent) {
    return {};
  }
  int bottom;
  // 饰品分组需通过下一个分组（妆容）的相对位置计算可见下界
  if (group.definition->id == "accessories") {
    auto next = locate(image, *nav.catalog.groups[2].entry, root_search_rect);
    if (!next || next->point.y - parent->point.y < accessories_min_parent_gap) {
      return {};
    }
    bottom = next->point.y - next_parent_bottom_inset;
  } else {
    bottom = parent->point.y + group.definition->viewport_bottom_offset;
    if (bottom > menu_rect[3]) {
      return {};
    }
  }
  // 物理展开区仍可能包含底部渐隐。定位、漏识别保护和滚动规划共用可读边界。
  bottom -= group.definition->bottom_fade_inset;
  if (bottom - (parent->point.y + child_top_offset) < child_min_height) {
    return {};
  }
  return scan::Box{child_search_x[0], parent->point.y + child_top_offset, child_search_x[1],
                   bottom};
}

// 计算图标按钮选中指示高光与周围背景的亮度对比度
auto contrast(const cv::Mat& pixels, cv::Point point, bool child) -> double {
  cv::Mat image;
  gray(pixels).convertTo(image, CV_32F);
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

// 判定指定分类或分组图标当前是否处于选中高亮状态
auto selected(const core::AppState& state, const Group& group, const Icon& icon,
              const cv::Mat& image, bool root = false) -> bool {
  bool child = group.entry.has_value() && !root;
  // 确定搜索区域边界
  auto area = child ? bounds(state, group, image) : std::optional{root_search_rect};
  if (!area) {
    return false;
  }
  // 定位图标并比对边缘选中高光对比度阈值
  auto match = locate(image, icon, *area, child);
  return match && contrast(image, match->point, child) >= selection_contrast;
}

// 计算图标理论预期坐标点位置
auto expected_point(const Group& group, const Icon& icon, const cv::Mat& image,
                    const scan::Box& area) -> std::expected<std::optional<cv::Point>, core::Error> {
  const auto& entries = group.categories;
  auto found = std::ranges::find_if(
      entries, [&](const Icon& item) { return item.definition->id == icon.definition->id; });
  int index = static_cast<int>(found - entries.begin());
  std::vector<int> neighbors;
  // 收集邻近同组图标索引
  for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
    if (i != index) {
      neighbors.push_back(i);
    }
  }
  std::ranges::sort(neighbors, [index](int a, int b) {
    return std::pair{std::abs(a - index), a} < std::pair{std::abs(b - index), b};
  });
  for (int i : neighbors) {
    auto match = locate(image, entries[i], area, true);
    if (!match) {
      continue;
    }
    cv::Point expected{match->point.x, match->point.y + (index - i) * child_row_step};
    if (area[1] + child_readable_inset <= expected.y &&
        expected.y < area[3] - child_readable_inset) {
      return std::unexpected(
          core::error("error.navigation.unmatched_readable",
                      core::text_key(std::format("category.{}", icon.definition->id))));
    }
    return std::optional{expected};
  }
  return std::optional<cv::Point>{};
}

// 计算滚动目标距离所需的滚轮格数
auto notches_for(int distance, int rate, int limit) -> int {
  return std::clamp(static_cast<int>(std::ceil(static_cast<double>(distance) / rate)), 1, limit);
}

// 滚动寻找视野外的分类或分组图标：循环滚动微调 → 等待画面静止 → 匹配定位图标中心
auto seek(core::AppState& state, const Group& group, const Icon& icon, int direction = 1,
          bool root = false) -> std::expected<Match, core::Error> {
  auto& nav = *state.navigation;
  bool child = group.entry.has_value() && !root;
  nav.stage = core::message(
      "navigation.stage.seeking", core::text_key(std::format("group.{}", group.definition->id)),
      core::text_key(std::format("{}.{}", root ? "group" : "category", icon.definition->id)));
  // 限制最大滚轮尝试次数，防止无限循环
  for (int count = 0; count <= max_menu_scrolls; ++count) {
    if (auto ok = window::check(state, state.scan->resources.target); !ok) {
      return std::unexpected(ok.error());
    }
    const auto& image = nav.last_frame->bgr;
    auto area = child ? bounds(state, group, image) : std::optional{root_search_rect};
    if (!area) {
      return std::unexpected(
          core::error("error.navigation.bounds_unknown",
                      core::text_key(std::format("group.{}", group.definition->id))));
    }
    // 当前视野若已能直接匹配到图标，则立即返回
    if (auto match = locate(image, icon, *area, child)) {
      return *match;
    }
    std::optional<cv::Point> expected;
    // 子分类通过同组其他已识别项推算目标图标的大致物理位置
    if (child) {
      auto result = expected_point(group, icon, image, *area);
      if (!result) {
        return std::unexpected(result.error());
      }
      expected = *result;
    }
    if (count == max_menu_scrolls) {
      break;
    }
    // 规划本次滚轮输入点与格数
    cv::Point point{outer_scroll_point[0], outer_scroll_point[1]};
    int notches = root_seek_notches;
    if (child) {
      point = {outer_scroll_point[0], ((*area)[1] + (*area)[3]) / 2};
      int limit = std::max(1, ((*area)[3] - (*area)[1] - child_row_step) / wheel_pixels_per_notch);
      int distance = expected ? direction * (expected->y - ((*area)[1] + child_target_top_offset))
                              : child_seek_notches * wheel_pixels_per_notch;
      if (distance <= 0) {
        return std::unexpected(
            core::error("error.navigation.wrong_direction",
                        core::text_key(std::format("category.{}", icon.definition->id))));
      }
      notches = notches_for(distance, wheel_pixels_per_notch, limit);
    }
    // 注入滚轮输入并等待画面静止
    auto sent = send_input(state, point, direction * notches);
    if (!sent) {
      return std::unexpected(sent.error());
    }
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

// 初始化导航会话：停靠鼠标 → 确认左侧菜单处于顶部就绪状态
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
  // 若子分类视口尚未完整露出来，向下滚动微调视口
  for (int count = 0; !bounds(state, group, nav.last_frame->bgr); ++count) {
    if (count == max_menu_scrolls) {
      return std::unexpected(core::error("error.navigation.reveal_limit"));
    }
    auto parent = locate(nav.last_frame->bgr, *group.entry, root_search_rect);
    if (!parent) {
      return std::unexpected(
          core::error("error.navigation.parent_lost",
                      core::text_key(std::format("group.{}", group.definition->id))));
    }
    int distance = parent->point.y - group.definition->expanded_parent_y;
    if (distance <= 0) {
      return std::unexpected(core::error("error.navigation.child_bounds_unknown"));
    }
    nav.stage = core::message("navigation.stage.revealing",
                              core::text_key(std::format("group.{}", group.definition->id)));
    int notches = notches_for(distance, wheel_pixels_per_notch, reveal_max_notches);
    auto boundary = send_input(state, {outer_scroll_point[0], outer_scroll_point[1]}, notches);
    if (!boundary) {
      return std::unexpected(boundary.error());
    }
    auto moved = wait_navigation(state, *boundary);
    if (!moved) {
      return std::unexpected(moved.error());
    }
  }
  auto area = *bounds(state, group, nav.last_frame->bgr);
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
    auto area = bounds(state, group, nav.last_frame->bgr);
    auto refreshed =
        area ? locate(nav.last_frame->bgr, icon, *area, group.entry.has_value()) : std::nullopt;
    if (!refreshed) {
      return std::unexpected(core::error("error.navigation.target_lost_after_park"));
    }
    *match = *refreshed;
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
  auto area = bounds(state, group, nav.last_frame->bgr);
  auto actual =
      area ? locate(nav.last_frame->bgr, icon, *area, group.entry.has_value()) : std::nullopt;
  if (!actual) {
    return std::unexpected(core::error("error.navigation.target_lost_after_selection"));
  }
  return {};
}

}  // namespace features::navigation

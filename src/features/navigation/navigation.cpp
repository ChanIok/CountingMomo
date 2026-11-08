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
// 子视口滚到边界后，多出来的滚轮会带动外层，使父入口偏离展开锚点。
// 展开态的外层滚动被游戏夹在「顶部仍保留最上面那个分类」的位置：父入口最低
// 只能到 expanded_parent_y，再往下滚也停在原地。所以这里不按像素率算格数，
// 一次给足格数让它自己撞到下界；没撞到再补一轮，仍不就位就报错。
// 偏移来自子视口回顶多出的那几格，实测不超过约 128px；按 16px/格 取 8 格即可，
// 撞界后多余的滚动被游戏吸收，不会把父入口推过头。
constexpr int align_notches = 8;
constexpr int align_max_attempts = 4;
// 下界落点允差：游戏把图标停在行心上，拟合有 1~2px 抖动。
// 不能放宽太多——妆容 bounds 的父行上限是 206，锚点 204 只剩 2px 余量。
constexpr int align_tolerance = 2;
constexpr int max_menu_scrolls = 30;
// 归位用分段向上滚：每轮只发 40 格，滚完重新拟合原点；到顶即停，没到再补一轮。
// 40 格按历史实测 16px/格约 640px，刚好覆盖「妆容展开」时 633px 的最远行程；
// 若滚轮速率更低，多轮累积也不会像固定 120 格那样出现整轮空转。
constexpr int reset_notches = 40;
constexpr int reset_max_attempts = 5;
// 回顶态下 hair 行中心（= 布局原点）的实测 y；允许几像素偏差，避免顶部
// 实际落点与实测值差 3~5px 就被判「没到顶」、白跑一整轮。
constexpr int outer_top_origin = 144;
constexpr int outer_top_tolerance = 8;
// 逐格滚轮之间的间隔（秒）：实机 0.016（约一帧）可让游戏逐条同步点击命中表。
// pause() 以 10ms 为轮询粒度，实际间隔取其整数倍。
constexpr double wheel_step_interval = 0.016;
constexpr double hover_seconds = 0.15;
constexpr double click_seconds = 0.12;
constexpr auto input_check_interval = std::chrono::milliseconds(10);

constexpr std::array<int, 2> child_search_x{125, 181};
constexpr int child_top_offset = 29;
constexpr int child_min_height = 200;
constexpr int next_parent_bottom_inset = 25;
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
// 饰品子视口高度：饰品行到展开态妆容入口的间距，列表底边不再越过它。
constexpr int accessories_viewport_height = expanded_makeup_offset - root_offsets[8];
// 妆容入口收起时比饰品行低 447、展开后低 693，取中点判断它有没有被挤下去。
// 这条判据只在子项列表认不出时兜底，正常路径以子项列表本身为准。
constexpr int expanded_makeup_midpoint = (root_offsets.back() + expanded_makeup_offset) / 2;
constexpr int layout_anchor_tolerance = 2;
constexpr int layout_min_anchors = 2;
constexpr int layout_probe_radius = 3;  // 推算中心的取整误差补偿窗口
// 子项列表至少要露出两行才值得投票：一行行距、两个图标半高，再加取整补偿。
constexpr int child_list_probe_height = child_row_step + 2 * child_half_size + layout_probe_radius;
// 亮框采样需要完整的 ±3px 窗口，比「能点到」更严格；hair 行太靠上时点得到却量不到高亮。
constexpr int root_layout_min_y =
    menu_rect[1] + root_half_size + selection_band_half + layout_probe_radius;
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
static_assert(root_ids[1] == "hair" && root_ids[2] == "dresses" && root_ids[8] == "accessories" &&
              root_ids[9] == "makeup" && root_offsets[2] > root_offsets[1] &&
              root_layout_min_y > menu_rect[1] + root_half_size);
static_assert(expanded_makeup_midpoint > root_offsets.back() &&
              expanded_makeup_midpoint < expanded_makeup_offset &&
              child_list_probe_height > child_row_step);
static_assert(calibration_notches > 0 && calibration_notches <= window::max_wheel_notches &&
              root_seek_notches > 0 && root_seek_notches <= window::max_wheel_notches &&
              child_seek_notches > 0 && child_seek_notches <= window::max_wheel_notches &&
              reveal_max_notches > 0 && reveal_max_notches <= window::max_wheel_notches &&
              align_notches > 0 && align_notches <= window::max_wheel_notches &&
              reset_notches > 0 && reset_notches <= window::max_wheel_notches &&
              reset_max_attempts > 0 && align_max_attempts > 0);
static_assert(min_wheel_rate > 0 && max_wheel_rate > min_wheel_rate);
// 最远行程是「妆容展开」态：hair 原点 = 妆容父行 − 展开偏移，到顶部要走 633px。
// 多轮累计必须覆盖它，不再按收起态的 388px 估算。
static_assert(static_cast<double>(reset_max_attempts * reset_notches) * min_wheel_rate >
              outer_top_origin - (groups[2].expanded_parent_y - expanded_makeup_offset));
static_assert(outer_top_origin > root_layout_min_y);
static_assert(wheel_step_interval >= 0.0 && wheel_step_interval <= 0.1);
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

// 把多格滚轮拆成逐条标准消息：一条消息只带一格。游戏若按「每条滚轮消息同步一次
// 点击命中表」实现，一条塞满 N 格的大 delta 会让画面滚到位而命中表停在旧位置，
// 之后按画面坐标点击就会落到别的分类。所有向上滚都由 send_scroll() 走这里；
// 向下滚仍保持原来的单条多格消息。
auto send_wheel_steps(core::AppState& state, cv::Point point, int notches)
    -> std::expected<std::int64_t, core::Error> {
  auto& nav = *state.navigation;
  if (notches == 0 || notches < -window::max_wheel_notches || notches > window::max_wheel_notches) {
    return std::unexpected(core::error("error.window.invalid_wheel_amount"));
  }
  if (auto ok = window::position_pointer(state, state.scan->resources.target, {point.x, point.y});
      !ok) {
    return std::unexpected(ok.error());
  }
  nav.pointer_parked = false;
  // 与 send_input() 的滚轮路径一致：先让悬停稳定，再开始注入。
  if (auto ok = pause(state, hover_seconds); !ok) {
    return std::unexpected(ok.error());
  }
  int step = notches > 0 ? 1 : -1;
  int count = std::abs(notches);
  for (int i = 0; i < count; ++i) {
    if (auto sent = window::wheel(state, state.scan->resources.target, step); !sent) {
      return std::unexpected(sent.error());
    }
    if (wheel_step_interval > 0.0 && i + 1 < count) {
      if (auto ok = pause(state, wheel_step_interval); !ok) {
        return std::unexpected(ok.error());
      }
    }
  }
  return capture::source_time();
}

// 统一的滚轮入口：向上滚一律逐格发送，向下滚沿用原来的单条多格输入。
// 子分类视口（饰品/妆容）里滚回首个分类、标定的回程，与外层过量回顶同样属于
// 向上滚，命中表错位的风险一致，因此在这里集中拦截，避免各调用点各写一套。
auto send_scroll(core::AppState& state, cv::Point point, int notches)
    -> std::expected<std::int64_t, core::Error> {
  // 0 格在 send_input() 里代表点击，不能误落到滚轮入口。
  if (notches == 0) {
    return std::unexpected(core::error("error.window.invalid_wheel_amount"));
  }
  return notches < 0 ? send_wheel_steps(state, point, notches) : send_input(state, point, notches);
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
  bool expanded{};      // 饰品子项列表已展开（妆容入口随之整体下移）
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

auto find_group(const Catalog& catalog, std::string_view id) -> const Group* {
  for (const auto& group : catalog.groups) {
    if (group.definition->id == id) {
      return &group;
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

// 用若干「行号 → 实测中心 y」的锚点投票求列表原点：行距恒定，取中位数后统计落在
// 容差内的票数。票数不足说明有图标被误认，宁可判「未知」也不给出错误原点。
auto vote_origin(std::vector<int> origins, int min_anchors) -> std::optional<int> {
  if (static_cast<int>(origins.size()) < min_anchors) {
    return {};
  }
  std::ranges::sort(origins);
  int median = origins[origins.size() / 2];
  int consistent = static_cast<int>(std::ranges::count_if(origins, [median](int value) {
    return std::abs(value - median) <= layout_anchor_tolerance;
  }));
  if (consistent < min_anchors) {
    return {};
  }
  return median;
}

// 子项列表是否真的挂在父入口下方：这是「分组已展开」最直接的证据。列表整体跟着
// 父行移动，界面上下抖十几像素不影响判别；妆容入口却压在整列末尾，随时可能被
// 根图标搜索窗口裁掉，不能作为唯一依据。
// 重新展开分组时首个分类默认被选中（发饰），图标带高亮底、模板得分偏低，所以优先
// 用后面的子项（帽子、耳饰……）投票，首项放到最后补位；两票一致即可收工，
// 正常路径只要两三次模板匹配。
auto child_list_visible(const Group& group, const cv::Mat& image, int parent_y) -> bool {
  const int top = std::max(parent_y + child_top_offset, menu_rect[1]);
  const int viewport_bottom = parent_y + accessories_viewport_height - next_parent_bottom_inset -
                              group.definition->bottom_fade_inset;
  const int bottom = std::min(viewport_bottom, menu_rect[3]);
  if (bottom - top < child_list_probe_height) {
    return false;
  }
  const scan::Box area{child_search_x[0], top, child_search_x[1], bottom};
  const int count = static_cast<int>(group.categories.size());
  std::vector<int> origins;
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

// 用所有可识别的根图标投票拟合菜单原点，再逐行量高亮，得出「当前亮的是第几项」。
// 收起与饰品展开两套排布只在最后一行的妆容入口上不同；妆容又常常被搜索窗口裁掉。
// 因此原点只用不受展开状态影响的行投票，展开与否直接看子项列表在不在：点击引起的
// 滚动、子类别收起都只改变原点、不改变行距，每帧重新拟合即可，无需认出目标图标。
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
  const int makeup_row = static_cast<int>(root_offsets.size()) - 1;
  std::vector<int> stable_origins;
  std::optional<int> makeup_y;
  for (const auto& [row, y] : anchors) {
    // 只有妆容行会随饰品展开而下移，其余行在两套排布里位置相同、推原点没有二义性。
    if (row == makeup_row) {
      makeup_y = y;
    } else {
      stable_origins.push_back(y - root_offsets[row]);
    }
  }
  int origin{};
  bool expanded{};
  if (auto voted = vote_origin(std::move(stable_origins), layout_min_anchors)) {
    origin = *voted;
    const int accessories_row = root_row_of("accessories");
    if (const auto* accessories = find_group(catalog, "accessories");
        accessories && accessories_row >= 0) {
      expanded = child_list_visible(*accessories, image, origin + root_offsets[accessories_row]);
    }
    // 子项列表整个滚出可识别范围时，退回「妆容入口被挤下去」这条旧判据兜底。
    if (!expanded && makeup_y) {
      expanded = *makeup_y - origin > expanded_makeup_midpoint;
    }
  } else {
    // 可认出的行太少（例如只剩妆容与相邻一行）：仍按两套排布分别拟合投票。
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
  RootLayout layout{.origin = origin, .expanded = expanded};
  for (int row = 0; row < static_cast<int>(root_offsets.size()); ++row) {
    layout.points[row] = cv::Point{outer_scroll_point[0], origin + root_offset(row, expanded)};
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
  // 饰品视口存不存在由子项列表本身判定；收起时妆容入口仍停在收起位置，不能当视口底。
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
  return vote_origin(std::move(origins), child_origin_min_anchors);
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
  auto down = send_scroll(state, point, calibration_notches);
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
  auto up = send_scroll(state, point, -calibration_notches);
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

// 子视口滚到边界后，多出来的滚轮会把外层往上带，父入口因此只可能偏低。
// 往下滚把父入口推回锚点：游戏在锚点处夹住滚动（顶部仍保留最上面那个分类），
// 滚过量也只是停在原地，所以不测量位移、不按像素率算格数，给足格数撞下界即可。
auto align_parent(core::AppState& state, const Group& group) -> std::expected<void, core::Error> {
  auto& nav = *state.navigation;
  if (!group.entry) {
    return {};
  }
  int row = root_row_of(group.entry->definition->id);
  auto lost = [&] {
    return std::unexpected(
        core::error("error.navigation.parent_lost",
                    core::text_key(std::format("group.{}", group.definition->id))));
  };
  if (row < 0) {
    return lost();
  }
  const cv::Point point{outer_scroll_point[0], outer_scroll_point[1]};
  for (int attempt = 0; attempt < align_max_attempts; ++attempt) {
    auto layout = infer_root_layout(state, nav.last_frame->bgr, gray_float(nav.last_frame->bgr));
    if (!layout) {
      return lost();
    }
    // 已在锚点（或更高）就不动：偏高只会让视口余量变大，不影响有效性与采集。
    if (layout->points[row].y <= group.definition->expanded_parent_y + align_tolerance) {
      return {};
    }
    auto sent = send_scroll(state, point, align_notches);
    if (!sent) {
      return std::unexpected(sent.error());
    }
    auto frame = wait_navigation(state, *sent);
    if (!frame) {
      return std::unexpected(frame.error());
    }
  }
  return std::unexpected(
      core::error("error.navigation.child_bounds_unknown",
                  core::text_key(std::format("group.{}", group.definition->id))));
}

// 分段向上滚外层菜单，直到落在回顶原点（顶部边界）。
// 每轮只发 40 格，滚完重新拟合原点；到顶即停，未到再补一轮，避免固定大格数空转。
// 逐格发送标准滚轮消息，避免大 delta 打乱游戏的点击命中表。
auto rewind_outer(core::AppState& state) -> std::expected<void, core::Error> {
  auto& nav = *state.navigation;
  const cv::Point point{outer_scroll_point[0], outer_scroll_point[1]};
  for (int attempt = 0; attempt < reset_max_attempts; ++attempt) {
    const auto& before_image = nav.last_frame->bgr;
    auto before = infer_root_layout(state, before_image, gray_float(before_image));
    auto sent = send_scroll(state, point, -reset_notches);
    if (!sent) {
      return std::unexpected(sent.error());
    }
    auto frame = wait_navigation(state, *sent);
    if (!frame) {
      return std::unexpected(frame.error());
    }
    const auto& after_image = nav.last_frame->bgr;
    auto after = infer_root_layout(state, after_image, gray_float(after_image));
    // 回顶后仍认不出整列排布，说明界面根本不在图鉴检索页，立即停止注入输入。
    if (!after) {
      return std::unexpected(core::error("error.navigation.menu_not_found"));
    }
    // 一轮就落在实测回顶原点，直接收工；不必再发一整轮滚轮去确认「原点不再移动」。
    if (std::abs(after->origin - outer_top_origin) <= outer_top_tolerance) {
      return {};
    }
    // 兜底：个别界面态回顶原点略有差异时，原点不再移动同样说明已停在顶部边界。
    // 首帧锚点不足时再滚一次，仍不收敛则不带病继续。
    if (before && std::abs(after->origin - before->origin) <= layout_anchor_tolerance) {
      return {};
    }
  }
  return std::unexpected(core::error("error.navigation.menu_not_found"));
}

// 按当前帧排布推算的根行坐标点击。选中态图标可能认不出，因此不用模板匹配定位。
auto click_root_row(core::AppState& state, int row) -> std::expected<void, core::Error> {
  auto& nav = *state.navigation;
  const auto& image = nav.last_frame->bgr;
  auto layout = infer_root_layout(state, image, gray_float(image));
  if (!layout || row < 0 || row >= static_cast<int>(layout->points.size())) {
    return std::unexpected(core::error("error.navigation.menu_not_found"));
  }
  cv::Point point = layout->points[row];
  if (point.y - root_half_size < menu_rect[1] || point.y + root_half_size >= menu_rect[3]) {
    return std::unexpected(core::error("error.navigation.menu_not_found"));
  }
  auto sent = send_input(state, point);
  if (!sent) {
    return std::unexpected(sent.error());
  }
  // 点击后的第一帧稳定画面即重新拟合，子类别收起引起的滚动变化不会沿用旧坐标。
  auto frame = wait_navigation(state, *sent);
  if (!frame) {
    return std::unexpected(frame.error());
  }
  return {};
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
  bool retried_bounds = false;
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
      // 子视口滚到边界时多出来的滚轮会带动外层，父入口被推到视口外才会走到这里。
      // 不逐次比对，等偏移真的把视口顶没了，再补一次把父入口送回锚点重算；
      // 真的收起了、或送回去仍然算不出，才报错。
      if (child && !retried_bounds) {
        retried_bounds = true;
        if (auto fixed = align_parent(state, group); !fixed) {
          return std::unexpected(fixed.error());
        }
        continue;
      }
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
      // 物理可见即返回；可读边距只属于滚动规划，不否决定位结果。
      if (expected->y - half >= top && expected->y + half < bottom) {
        return Match{*expected};
      }
    }
    if (count == max_menu_scrolls) {
      break;
    }
    // 方向由目标推算位置决定：在视口上方就向上滚，在下方就向下滚。子列表被滚到底、
    // 点击收起子类别引起外层位移等情况都能自行收敛；只有连两个锚点都凑不出推算坐标时，
    // 才退回调用方给的提示方向做一次粗滚。
    int scroll_direction = direction;
    if (expected) {
      int ideal = child ? (*area)[1] + child_target_top_offset : (menu_rect[1] + menu_rect[3]) / 2;
      scroll_direction = expected->y > ideal ? 1 : -1;
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
      int distance = expected ? std::abs(expected->y - ((*area)[1] + child_target_top_offset))
                              : static_cast<int>(child_seek_notches * rate);
      notches = notches_for(distance, rate, limit);
    }
    // 注入滚轮输入并等待画面静止
    auto sent = send_scroll(state, point, scroll_direction * notches);
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

// 初始化导航会话：按当前这一步需要什么处理状态，干净状态下不注入任何输入。
// 正式扫描的第一步 visit(hair) 本身就是「找发型 → 点击 → 等选中」；只有起步就已选中 hair
// 时它会跳过点击、右侧列表不会回顶，因此这里点一次连衣裙打破该状态。
// 外层若被用户滚过，先过量回顶让 hair 行进入亮框采样窗口；子类别由点击 hair 自行收起。
auto begin(core::AppState& state) -> std::expected<void, core::Error> {
  auto& nav = *state.navigation;
  if (auto ok = park(state); !ok) {
    return ok;
  }
  nav.stage = core::message("navigation.stage.waiting_initial");
  // 取首帧稳定画面；此后每个动作都基于最新帧重新拟合排布，不沿用旧坐标。
  auto initial = wait_navigation(state, capture::source_time());
  if (!initial) {
    return std::unexpected(initial.error());
  }
  const int hair_row = root_row_of("hair");
  auto inspect = [&](const cv::Mat& image) -> std::optional<RootLayout> {
    return infer_root_layout(state, image, gray_float(image));
  };
  // 认不出排布、或 hair 行太靠边导致亮框采样窗口不完整，才需要回顶。
  auto usable = [&](const std::optional<RootLayout>& layout) -> bool {
    return layout && hair_row >= 0 && layout->points[hair_row].y >= root_layout_min_y;
  };
  auto layout = inspect(nav.last_frame->bgr);
  if (!usable(layout)) {
    nav.stage = core::message("navigation.stage.resetting");
    if (auto reset = rewind_outer(state); !reset) {
      return reset;
    }
    layout = inspect(nav.last_frame->bgr);
    if (!usable(layout)) {
      return std::unexpected(core::error("error.navigation.menu_not_found"));
    }
  }
  // 只有起步就选中 hair 需要额外输入；hair 可见时下一行连衣裙必定可点，不做上下二选一。
  if (layout->highlighted == hair_row) {
    nav.stage = core::message("navigation.stage.resetting");
    if (auto clicked = click_root_row(state, root_row_of("dresses")); !clicked) {
      return clicked;
    }
    // 只读已经等到的稳定帧，确认高亮确实离开 hair 行，避免 visit(hair) 再次跳过点击。
    auto after = inspect(nav.last_frame->bgr);
    if (!after || after->highlighted == hair_row) {
      return std::unexpected(core::error("error.navigation.menu_not_found"));
    }
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
    auto boundary = send_scroll(state, point, notches);
    if (!boundary) {
      return std::unexpected(boundary.error());
    }
    ++count;
    auto moved = wait_navigation(state, *boundary);
    if (!moved) {
      return std::unexpected(moved.error());
    }
  }
  // 子分类列表收起期间会保留上次的滚动位置；不做无条件回顶，交给 seek(first)
  // 按当前可见子项投票推算列表原点，再自行决定往上还是往下滚。
  auto area = bounds(group, *layout);
  if (!area) {
    return std::unexpected(core::error("error.navigation.child_bounds_unknown"));
  }
  // 确保展开区域内至少能识别到任一子分类图标
  if (!std::ranges::any_of(group.categories, [&](const Icon& icon) {
        return locate(nav.last_frame->bgr, icon, *area, true).has_value();
      })) {
    return std::unexpected(core::error("error.navigation.no_child_icons"));
  }
  // 将首个目标分类滚到可读区，不在定位阶段点击。
  auto first = seek(state, group, group.categories.front(), -1);
  if (!first) {
    return std::unexpected(first.error());
  }
  // 子视口滚到顶端后，多出来的滚轮会推动外层，把父入口从展开锚点带走。
  // 回顶结束后补一次对齐，否则这段偏移会一直留到后续分类的视口推算里。
  return align_parent(state, group);
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

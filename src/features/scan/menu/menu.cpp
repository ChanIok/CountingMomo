#include "features/scan/menu/menu.hpp"

#include "vendor/std.hpp"

#include "vendor/opencv.hpp"

#include "core/message/message.hpp"
#include "core/state/app_state.hpp"
#include "features/scan/capture/capture.hpp"
#include "features/scan/layout.hpp"
#include "features/scan/menu/constants.hpp"
#include "features/scan/menu/vision.hpp"
#include "features/scan/runtime.hpp"
#include "features/scan/state.hpp"
#include "features/window/window.hpp"
#include "utils/file.hpp"
#include "utils/image.hpp"
#include "utils/string.hpp"
#include "utils/timer.hpp"

namespace features::scan::menu {

namespace {

constexpr std::array<int, 2> park_point{193, 60};

constexpr int calibration_notches = 2;
constexpr int calibration_min_matches = 2;
constexpr int calibration_shift_tolerance = 2;
constexpr double min_wheel_rate = 4.0;
constexpr double max_wheel_rate = 128.0;
constexpr int root_seek_notches = 6;
constexpr int child_seek_notches = 6;
constexpr int reveal_max_notches = 24;

constexpr int align_notches = 8;
constexpr int align_max_attempts = 4;
constexpr int align_tolerance = 2;

constexpr int max_menu_scrolls = 30;

constexpr int reset_notches = 40;
constexpr int reset_max_attempts = 5;

constexpr int outer_top_origin = 144;
constexpr int outer_top_tolerance = 8;

constexpr double wheel_step_interval = 0.017;

constexpr double hover_seconds = 0.15;
constexpr double click_seconds = 0.12;
constexpr auto input_check_interval = std::chrono::milliseconds(10);

constexpr int child_target_top_offset = 53;

// 在指定时间内暂停：分片休眠 → 持续检查游戏窗口有效性与停止请求
auto pause(core::AppState& state, double duration) -> std::expected<void, core::Error> {
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                      std::chrono::duration<double>(duration));
  // 分片休眠，循环检测停止状态与窗口有效性
  while (true) {
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return {};
    }
    // 检查游戏窗口是否失焦或被关闭
    if (auto ok = window::check(state, state.scan->session.resources.target); !ok) {
      return ok;
    }
    // 休眠一小片时间，避免系统定时器误差放大
    auto slice = std::min(deadline, now + input_check_interval);
    if (auto slept = utils::timer::sleep_until(slice); !slept) {
      return std::unexpected(slept.error());
    }
  }
}

// 将鼠标移至安全停泊区，避免悬停触发菜单项伪高亮
auto park(core::AppState& state) -> std::expected<void, core::Error> {
  auto& menu = state.scan->session.menu;
  auto point = park_point;
  // 移动光标至无交互的空白泊位
  if (auto ok = window::position_pointer(state, state.scan->session.resources.target,
                                         {point[0], point[1]});
      !ok) {
    return ok;
  }
  // 标记鼠标已停放
  menu.pointer_parked = true;
  return {};
}

// 发送点击或多格滚轮输入：定位光标 → 稍作悬停 → 发送输入 → 点击后停放鼠标
auto send_input(core::AppState& state, cv::Point point, int notches = 0)
    -> std::expected<std::int64_t, core::Error> {
  auto& menu = state.scan->session.menu;
  // 移动光标到交互目标点
  if (auto ok =
          window::position_pointer(state, state.scan->session.resources.target, {point.x, point.y});
      !ok) {
    return std::unexpected(ok.error());
  }
  menu.pointer_parked = false;
  // 短暂等待悬停就位
  if (auto ok = pause(state, hover_seconds); !ok) {
    return std::unexpected(ok.error());
  }
  // 发送物理点击或滚轮消息
  auto sent = notches ? window::wheel(state, state.scan->session.resources.target, notches)
                      : window::click(state, state.scan->session.resources.target);
  if (!sent) {
    return std::unexpected(sent.error());
  }
  // 点击操作后等待并移走鼠标，消除悬停高亮干扰
  if (!notches) {
    if (auto ok = pause(state, click_seconds); !ok) {
      return std::unexpected(ok.error());
    }
    if (auto ok = park(state); !ok) {
      return std::unexpected(ok.error());
    }
  }
  // 记录输入完成时刻的时间戳
  return capture::source_time();
}

// 逐格拆分发送滚轮消息，防止游戏点击命中表与滚动画面脱节
auto send_wheel_steps(core::AppState& state, cv::Point point, int notches)
    -> std::expected<std::int64_t, core::Error> {
  auto& menu = state.scan->session.menu;
  // 校验滚轮步数合法性
  if (notches == 0 || notches < -window::max_wheel_notches || notches > window::max_wheel_notches) {
    return std::unexpected(core::error("error.window.invalid_wheel_amount"));
  }
  // 移动光标到滚动区域
  if (auto ok =
          window::position_pointer(state, state.scan->session.resources.target, {point.x, point.y});
      !ok) {
    return std::unexpected(ok.error());
  }
  menu.pointer_parked = false;
  // 短暂等待悬停就位
  if (auto ok = pause(state, hover_seconds); !ok) {
    return std::unexpected(ok.error());
  }
  int step = notches > 0 ? 1 : -1;
  int count = std::abs(notches);
  // 按单格步长依次发送滚轮消息，每步插入微小延时
  for (int i = 0; i < count; ++i) {
    if (auto sent = window::wheel(state, state.scan->session.resources.target, step); !sent) {
      return std::unexpected(sent.error());
    }
    if (wheel_step_interval > 0.0 && i + 1 < count) {
      if (auto ok = pause(state, wheel_step_interval); !ok) {
        return std::unexpected(ok.error());
      }
    }
  }
  // 记录输入完成时刻的时间戳
  return capture::source_time();
}

// 统一滚轮调度：向上滚动逐格发送，向下滚动批量发送
auto send_scroll(core::AppState& state, cv::Point point, int notches)
    -> std::expected<std::int64_t, core::Error> {
  // 滚轮格数不可为 0
  if (notches == 0) {
    return std::unexpected(core::error("error.window.invalid_wheel_amount"));
  }
  // 向上滚走逐格通道防错位，向下滚走快速通道
  return notches < 0 ? send_wheel_steps(state, point, notches) : send_input(state, point, notches);
}

// 等待画面稳定并满足导航断言：GPU 判稳 → 校验断言 → 超时返回阶段错误
auto wait_navigation(core::AppState& state, std::int64_t after_source_time,
                     const std::function<bool(const cv::Mat&)>& predicate = {},
                     std::string_view timeout_key = "error.navigation.timeout")
    -> std::expected<capture::Frame, core::Error> {
  auto& menu = state.scan->session.menu;
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                      std::chrono::duration<double>(menu.frame_timeout));
  bool require_change = false;
  // 循环等待直到超时或画面满足断言
  while (std::chrono::steady_clock::now() < deadline) {
    // 轮询等待 GPU 计算着色器判定的稳定帧
    auto frame = capture::wait_stable_frame(
        state, after_source_time, deadline,
        [&] { return window::check(state, state.scan->session.resources.target); }, require_change);
    if (!frame) {
      auto error = std::move(frame.error());
      error.related.push_back({menu.stage, {}, {}});
      return std::unexpected(std::move(error));
    }
    // 捕获超时
    if (!*frame) {
      return std::unexpected(core::Error{core::message(timeout_key), {}, {{menu.stage, {}, {}}}});
    }
    // 缓存最新帧
    menu.last_frame = *frame;
    // 若提供了自定义断言且未通过，要求画面再次发生变化后重试
    if (predicate && !predicate((**frame).bgr)) {
      after_source_time = (**frame).source_time;
      require_change = true;
      continue;
    }
    return std::move(**frame);
  }
  return std::unexpected(core::Error{core::message(timeout_key), {}, {{menu.stage, {}, {}}}});
}

// 标定滚轮像素比率：向下滚小步 → 计算图标位移中位数 → 原路滚回 → 校验并返回每格像素
auto calibrate_wheel(core::AppState& state, cv::Point point) -> std::expected<double, core::Error> {
  auto& menu = state.scan->session.menu;
  using Positions = std::vector<std::pair<const Icon*, int>>;
  // 识别候选图标在当前画面中的纵坐标
  auto positions_of = [](const cv::Mat& image, const Positions& reference) -> Positions {
    Positions result;
    for (const auto& item : reference) {
      if (auto match = locate(image, *item.first, root_search_rect)) {
        result.emplace_back(item.first, match->point.y);
      }
    }
    return result;
  };
  // 统计图标滚动前后纵坐标偏移量的中位数
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

  // 状态切为正在标定滚轮
  menu.stage = core::message("navigation.stage.calibrating");
  // 收集滚动前当前画面中可见的候选图标坐标
  Positions before;
  for (const auto& group : menu.catalog.groups) {
    if (group.entry) {
      if (auto match = locate(menu.last_frame->bgr, *group.entry, root_search_rect)) {
        before.emplace_back(&*group.entry, match->point.y);
      }
    }
    for (const auto& icon : group.categories) {
      if (auto match = locate(menu.last_frame->bgr, icon, root_search_rect)) {
        before.emplace_back(&icon, match->point.y);
      }
    }
  }
  // 可见参考图标不足，无法可靠标定
  if (before.size() < calibration_min_matches) {
    return std::unexpected(core::error("error.navigation.calibration_failed"));
  }
  // 向下发送固定小步滚轮
  auto down = send_scroll(state, point, calibration_notches);
  if (!down) {
    return std::unexpected(down.error());
  }
  // 等待滚动后画面稳定
  if (auto moved = wait_navigation(state, *down); !moved) {
    return std::unexpected(moved.error());
  }
  // 计算向下滚动导致的图标像素位移
  Positions after_down = positions_of(menu.last_frame->bgr, before);
  int down_shift = median_shift(before, after_down);
  // 原路向上滚回相同格数，恢复菜单原位
  auto up = send_scroll(state, point, -calibration_notches);
  if (!up) {
    return std::unexpected(up.error());
  }
  // 等待画面恢复原状
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
  // 停放鼠标消除悬停高亮
  if (auto parked = park(state); !parked) {
    return std::unexpected(parked.error());
  }
  // 计算回程位移并校验往返一致性与比率合理性
  int up_shift = -median_shift(after_down, positions_of(menu.last_frame->bgr, after_down));
  double rate = static_cast<double>(down_shift) / calibration_notches;
  if (down_shift <= 0 || std::abs(down_shift - up_shift) > calibration_shift_tolerance ||
      rate < min_wheel_rate || rate > max_wheel_rate) {
    return std::unexpected(core::error("error.navigation.calibration_failed"));
  }
  return rate;
}

// 确保滚轮像素比率已标定：若未标定则触发探测并提示调用方重算帧
auto ensure_wheel_rate(core::AppState& state, cv::Point point) -> std::expected<bool, core::Error> {
  auto& menu = state.scan->session.menu;
  // 已有标定结果，直接复用
  if (menu.pixels_per_notch > 0) {
    return false;
  }
  // 首次执行动态标定
  auto rate = calibrate_wheel(state, point);
  if (!rate) {
    return std::unexpected(rate.error());
  }
  // 记录标定比率并通知调用方重新拟合最新帧
  menu.pixels_per_notch = *rate;
  return true;
}

// 向下微调外层滚动，将偏低的父入口推回标准展开锚点
auto align_parent(core::AppState& state, const Group& group) -> std::expected<void, core::Error> {
  auto& menu = state.scan->session.menu;
  // 根分组无父入口，直接跳过
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
  // 循环尝试将父入口推至锚点
  for (int attempt = 0; attempt < align_max_attempts; ++attempt) {
    // 拟合当前菜单排布
    auto layout = infer_root_layout(state, menu.last_frame->bgr, gray_float(menu.last_frame->bgr));
    if (!layout) {
      return lost();
    }
    // 父入口已到达或高于展开锚点，对齐完成
    if (layout->points[row].y <= group.definition->expanded_parent_y + align_tolerance) {
      return {};
    }
    // 向下滚动固定格数推进父入口
    auto sent = send_scroll(state, point, align_notches);
    if (!sent) {
      return std::unexpected(sent.error());
    }
    // 等待滚动后画面稳定
    auto frame = wait_navigation(state, *sent);
    if (!frame) {
      return std::unexpected(frame.error());
    }
  }
  return std::unexpected(
      core::error("error.navigation.child_bounds_unknown",
                  core::text_key(std::format("group.{}", group.definition->id))));
}

// 分段向上滚动外层菜单，直至回到顶部基准原点
auto rewind_outer(core::AppState& state) -> std::expected<void, core::Error> {
  auto& menu = state.scan->session.menu;
  const cv::Point point{outer_scroll_point[0], outer_scroll_point[1]};
  // 分段循环向上回滚
  for (int attempt = 0; attempt < reset_max_attempts; ++attempt) {
    const auto& before_image = menu.last_frame->bgr;
    auto before = infer_root_layout(state, before_image, gray_float(before_image));
    // 向上发送一段滚轮
    auto sent = send_scroll(state, point, -reset_notches);
    if (!sent) {
      return std::unexpected(sent.error());
    }
    // 等待画面稳定
    auto frame = wait_navigation(state, *sent);
    if (!frame) {
      return std::unexpected(frame.error());
    }
    // 重新拟合回滚后的排布
    const auto& after_image = menu.last_frame->bgr;
    auto after = infer_root_layout(state, after_image, gray_float(after_image));
    // 菜单排布无法识别，说明界面异常
    if (!after) {
      return std::unexpected(core::error("error.navigation.menu_not_found"));
    }
    // 达到预设的顶部原点，回滚完成
    if (std::abs(after->origin - outer_top_origin) <= outer_top_tolerance) {
      return {};
    }
    // 连续两次原点未发生变化，说明已撞到物理顶部
    if (before && std::abs(after->origin - before->origin) <= layout_anchor_tolerance) {
      return {};
    }
  }
  return std::unexpected(core::error("error.navigation.menu_not_found"));
}

// 点击外层菜单的指定行：推算坐标 → 校验边界 → 发送点击并等待画面稳定
auto click_root_row(core::AppState& state, int row) -> std::expected<void, core::Error> {
  auto& menu = state.scan->session.menu;
  const auto& image = menu.last_frame->bgr;
  // 推算外层整列排布
  auto layout = infer_root_layout(state, image, gray_float(image));
  if (!layout || row < 0 || row >= static_cast<int>(layout->points.size())) {
    return std::unexpected(core::error("error.navigation.menu_not_found"));
  }
  // 检查目标行是否在菜单物理可见区内
  cv::Point point = layout->points[row];
  if (point.y - root_half_size < menu_rect[1] || point.y + root_half_size >= menu_rect[3]) {
    return std::unexpected(core::error("error.navigation.menu_not_found"));
  }
  // 发送点击输入
  auto sent = send_input(state, point);
  if (!sent) {
    return std::unexpected(sent.error());
  }
  // 等待点击后稳定画面
  auto frame = wait_navigation(state, *sent);
  if (!frame) {
    return std::unexpected(frame.error());
  }
  return {};
}

// 滚动寻找视野外的分类或分组图标：推算排布 → 定位目标 → 计算滚轮格数并滚动微调
auto seek(core::AppState& state, const Group& group, const Icon& icon, int direction = 1,
          bool root = false) -> std::expected<Match, core::Error> {
  auto& menu = state.scan->session.menu;
  bool child = group.entry.has_value() && !root;
  // 播报正在寻找目标分类或分组
  menu.stage = core::message(
      "navigation.stage.seeking", core::text_key(std::format("group.{}", group.definition->id)),
      core::text_key(std::format("{}.{}", root ? "group" : "category", icon.definition->id)));
  // 循环滚动调整，限制最大尝试次数
  bool retried_bounds = false;
  for (int count = 0; count <= max_menu_scrolls;) {
    // 检查游戏窗口状态
    if (auto ok = window::check(state, state.scan->session.resources.target); !ok) {
      return std::unexpected(ok.error());
    }
    const auto& image = menu.last_frame->bgr;
    auto flat = gray_float(image);
    // 拟合当前画面排布
    auto layout = infer_root_layout(state, image, flat);
    if (!layout) {
      return std::unexpected(
          core::error("error.navigation.bounds_unknown",
                      core::text_key(std::format("group.{}", group.definition->id))));
    }
    // 计算当前搜索视口边界
    auto area = child ? bounds(group, *layout) : std::optional{root_search_rect};
    if (!area) {
      // 子视口因父入口偏移导致超出边界时，尝试对齐父入口恢复视口
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
    // 尝试直接模板匹配常态图标
    if (auto match = locate(image, icon, *area, child)) {
      return *match;
    }
    // 模板匹配未中（可能处于选中态），改由排布几何推算目标坐标
    std::optional<cv::Point> expected = target_point(group, icon, image, *layout, root);
    if (expected) {
      int top = child ? (*area)[1] : menu_rect[1];
      int bottom = child ? (*area)[3] : menu_rect[3];
      int half = child ? child_half_size : root_half_size;
      // 目标已在可见视口内，直接返回推算坐标
      if (expected->y - half >= top && expected->y + half < bottom) {
        return Match{*expected};
      }
    }
    if (count == max_menu_scrolls) {
      break;
    }
    // 依据目标推算位置决定滚动方向，未推算出时采用入参默认方向
    int scroll_direction = direction;
    if (expected) {
      int ideal = child ? (*area)[1] + child_target_top_offset : (menu_rect[1] + menu_rect[3]) / 2;
      scroll_direction = expected->y > ideal ? 1 : -1;
    }
    // 规划滚轮输入点与格数
    cv::Point point{outer_scroll_point[0], outer_scroll_point[1]};
    int notches = root_seek_notches;
    if (child) {
      point = {outer_scroll_point[0], ((*area)[1] + (*area)[3]) / 2};
      // 确保子视口已具备滚轮像素比率
      auto calibrated = ensure_wheel_rate(state, point);
      if (!calibrated) {
        return std::unexpected(calibrated.error());
      }
      if (*calibrated) {
        continue;  // 刚完成标定，重新基于新帧定位与推算
      }
      // 根据目标与理想顶部的距离换算滚轮格数
      double rate = menu.pixels_per_notch;
      int limit = std::max(1, static_cast<int>(((*area)[3] - (*area)[1] - child_row_step) / rate));
      int distance = expected ? std::abs(expected->y - ((*area)[1] + child_target_top_offset))
                              : static_cast<int>(child_seek_notches * rate);
      notches = notches_for(distance, rate, limit);
    }
    // 注入滚轮输入并等待画面稳定
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

// 分类与分组图标的静态目录：仅用于资源加载，不对外暴露
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

// 加载分类与分组图标资源：读取 PNG 文件 → 校验尺寸 → 提取特征信号
auto load(const std::filesystem::path& directory) -> std::expected<Catalog, core::Error> {
  // 加载单张图标并提取边缘特征
  auto make_icon = [&](const IconDefinition& definition) -> std::expected<Icon, core::Error> {
    // 从磁盘加载图标 PNG 图片
    auto pixels =
        utils::image::load(directory / utils::string::wide(std::format("{}.png", definition.id)));
    if (!pixels) {
      return std::unexpected(pixels.error());
    }
    // 校验图片尺寸与定义是否一致
    if (pixels->cols != definition.size || pixels->rows != definition.size) {
      return std::unexpected(core::error("error.navigation.invalid_icon_geometry", definition.id));
    }
    // 提取高频边缘特征信号
    auto signal = icon_signal(*pixels);
    if (cv::norm(signal) == 0) {
      return std::unexpected(core::error("error.navigation.empty_icon", definition.id));
    }
    return Icon{&definition, std::move(signal)};
  };
  Catalog catalog;
  // 遍历所有分组加载入口及子分类图标
  for (const auto& definition : groups) {
    Group runtime{.definition = &definition};
    // 加载分组父级入口图标（若存在）
    if (definition.entry) {
      auto icon = make_icon(*definition.entry);
      if (!icon) {
        return std::unexpected(icon.error());
      }
      runtime.entry = std::move(*icon);
    }
    // 加载分组下属全部子分类图标
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

}  // namespace

// 加载分类图标资源并初始化菜单会话；失败由动词边界转成扫描中断
auto prepare(core::AppState& state, double frame_timeout) -> void {
  auto root = runtime::must(state, utils::file::executable_directory());
  auto catalog = runtime::must(state, load(root / "resources/categories"));
  auto& menu = state.scan->session.menu;
  menu.catalog = std::move(catalog);
  menu.frame_timeout = frame_timeout;
}

// 初始化菜单状态：停放鼠标 → 等待首帧 → 必要时回顶并打破发型初始选中
auto begin(core::AppState& state) -> std::expected<void, core::Error> {
  auto& menu = state.scan->session.menu;
  menu.stage = core::message("navigation.stage.initializing");
  // 停放鼠标至安全泊位
  if (auto ok = park(state); !ok) {
    return ok;
  }
  menu.stage = core::message("navigation.stage.waiting_initial");
  // 等待首帧稳定画面
  auto initial = wait_navigation(state, capture::source_time());
  if (!initial) {
    return std::unexpected(initial.error());
  }
  const int hair_row = root_row_of("hair");
  auto inspect = [&](const cv::Mat& image) -> std::optional<RootLayout> {
    return infer_root_layout(state, image, gray_float(image));
  };
  // 检验菜单排布是否完整且发型行落在有效采样区
  auto usable = [&](const std::optional<RootLayout>& layout) -> bool {
    return layout && hair_row >= 0 && layout->points[hair_row].y >= root_layout_min_y;
  };
  // 检查首帧排布，不可用则执行外层回顶重置
  auto layout = inspect(menu.last_frame->bgr);
  if (!usable(layout)) {
    menu.stage = core::message("navigation.stage.resetting");
    if (auto reset = rewind_outer(state); !reset) {
      return reset;
    }
    layout = inspect(menu.last_frame->bgr);
    if (!usable(layout)) {
      return std::unexpected(core::error("error.navigation.menu_not_found"));
    }
  }
  // 若发型行处于初始选中状态，点击连衣裙打破该状态以确保右侧列表刷新
  if (layout->highlighted == hair_row) {
    menu.stage = core::message("navigation.stage.resetting");
    if (auto clicked = click_root_row(state, root_row_of("dresses")); !clicked) {
      return clicked;
    }
    // 验证高亮已离开发型行
    auto after = inspect(menu.last_frame->bgr);
    if (!after || after->highlighted == hair_row) {
      return std::unexpected(core::error("error.navigation.menu_not_found"));
    }
  }
  return {};
}

// 展开目标分组：定位入口图标 → 点击展开 → 滚动露出子视口并对齐
auto open_group(core::AppState& state, const Group& group) -> std::expected<void, core::Error> {
  auto& menu = state.scan->session.menu;
  // 无父级入口的根组（如服装）直接跳过
  if (!group.entry) {
    return {};
  }
  // 定位分组父级入口图标
  auto match = seek(state, group, *group.entry, 1, true);
  if (!match) {
    return std::unexpected(match.error());
  }
  menu.stage = core::message("navigation.stage.opening",
                             core::text_key(std::format("group.{}", group.definition->id)));
  // 点击父级入口以展开分组
  auto sent = send_input(state, match->point);
  if (!sent) {
    return std::unexpected(sent.error());
  }
  // 等待展开并确认父级进入选中高亮状态
  auto frame = wait_navigation(state, *sent, [&](const cv::Mat& image) {
    return selected(state, group, *group.entry, image, true);
  });
  if (!frame) {
    return std::unexpected(frame.error());
  }
  // 若子视口未完全露出，向下滚动微调直到视口完整呈现
  std::optional<RootLayout> layout;
  for (int count = 0;;) {
    const auto& image = menu.last_frame->bgr;
    auto flat = gray_float(image);
    layout = infer_root_layout(state, image, flat);
    if (!layout) {
      return std::unexpected(
          core::error("error.navigation.parent_lost",
                      core::text_key(std::format("group.{}", group.definition->id))));
    }
    // 视口已完整露出，跳出调整循环
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
    menu.stage = core::message("navigation.stage.revealing",
                               core::text_key(std::format("group.{}", group.definition->id)));
    const cv::Point point{outer_scroll_point[0], outer_scroll_point[1]};
    // 确保外层滚轮比率可用
    auto calibrated = ensure_wheel_rate(state, point);
    if (!calibrated) {
      return std::unexpected(calibrated.error());
    }
    if (*calibrated) {
      continue;  // 探测后重新拟合排布
    }
    // 发送滚动并等待画面稳定
    double rate = menu.pixels_per_notch;
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
  // 校验子视口有效性
  auto area = bounds(group, *layout);
  if (!area) {
    return std::unexpected(core::error("error.navigation.child_bounds_unknown"));
  }
  // 校验展开区域内至少能识别到一个子分类图标
  if (!std::ranges::any_of(group.categories, [&](const Icon& icon) {
        return locate(menu.last_frame->bgr, icon, *area, true).has_value();
      })) {
    return std::unexpected(core::error("error.navigation.no_child_icons"));
  }
  // 滚动将第一个子分类拉入可见区
  auto first = seek(state, group, group.categories.front(), -1);
  if (!first) {
    return std::unexpected(first.error());
  }
  // 回顶后将可能发生微位移的父入口重新对齐至展开锚点
  return align_parent(state, group);
}

// 切换至指定分类：滚动定位图标 → 停放鼠标防伪高光 → 点击选中并确认生效
auto visit(core::AppState& state, const Group& group, const Icon& icon)
    -> std::expected<void, core::Error> {
  auto& menu = state.scan->session.menu;
  // 滚动并定位目标分类图标
  auto match = seek(state, group, icon);
  if (!match) {
    return std::unexpected(match.error());
  }
  menu.stage = core::message("navigation.stage.selecting",
                             core::text_key(std::format("group.{}", group.definition->id)),
                             core::text_key(std::format("category.{}", icon.definition->id)));
  // 若光标不在泊位，先移走鼠标以消除悬停伪高光，并重新校准目标坐标
  if (!menu.pointer_parked) {
    if (auto parked = park(state); !parked) {
      return parked;
    }
    // 等待鼠标停放后的画面稳定
    auto frame = wait_navigation(state, capture::source_time());
    if (!frame) {
      return std::unexpected(frame.error());
    }
    // 依据停放后的稳定帧重新推算目标坐标
    auto flat = gray_float(menu.last_frame->bgr);
    auto layout = infer_root_layout(state, menu.last_frame->bgr, flat);
    std::optional<cv::Point> refreshed;
    if (layout) {
      refreshed = target_point(group, icon, menu.last_frame->bgr, *layout, false);
    }
    if (!refreshed) {
      return std::unexpected(core::error("error.navigation.target_lost_after_park"));
    }
    match->point = *refreshed;
  }
  // 若目标未处于选中状态，发送点击并等待高亮确认
  if (!selected(state, group, icon, menu.last_frame->bgr)) {
    auto sent = send_input(state, match->point);
    if (!sent) {
      return std::unexpected(sent.error());
    }
    // 等待点击后目标分类进入选中高亮状态
    auto frame = wait_navigation(
        state, *sent, [&](const cv::Mat& image) { return selected(state, group, icon, image); });
    if (!frame) {
      return std::unexpected(frame.error());
    }
  }
  // 再次复核确认目标已处于选中状态
  if (!selected(state, group, icon, menu.last_frame->bgr)) {
    return std::unexpected(core::error("error.navigation.target_lost_after_selection"));
  }
  return {};
}

}  // namespace features::scan::menu

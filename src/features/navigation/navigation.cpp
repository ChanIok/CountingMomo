#include "features/navigation/navigation.hpp"

#include "vendor/std.hpp"

#include "vendor/rfl.hpp"

#include "features/scan/vision.hpp"
#include "utils/file.hpp"
#include "utils/image.hpp"
#include "utils/string.hpp"

namespace features::navigation {
namespace {
using Clock = std::chrono::steady_clock;
using Frame = std::shared_ptr<const capture::Frame>;
constexpr int max_menu_scrolls = 30;
struct CatalogDefinition {
  Settings settings;
  std::vector<GroupDefinition> groups;
};
struct Boundary {
  Frame before;
  Clock::time_point sent_at{};
};
struct Event {
  double at{};
  std::string stage;
  std::string action;
  std::string target;
  std::array<int, 2> point{};
  int notches{};
  std::uint64_t sequence{};
  std::int64_t source_time{};
  double score{};
  double margin{};
};
auto log(Navigator& nav, Event event) -> std::expected<void, std::string> {
  event.at = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
  event.stage = nav.stage;
  nav.journal << rfl::json::write(event) << '\n';
  nav.journal.flush();
  if (!nav.journal) return std::unexpected("分类导航日志写入失败。");
  return {};
}
auto gray(const cv::Mat& image) -> cv::Mat {
  cv::Mat result;
  cv::cvtColor(image, result, cv::COLOR_BGR2GRAY);
  return result;
}
auto signal(const cv::Mat& image) -> cv::Mat {
  cv::Mat value, blurred, result;
  gray(image).convertTo(value, CV_32F);
  cv::GaussianBlur(value, blurred, {0, 0}, 3);
  cv::subtract(value, blurred, result);
  cv::max(result, 0, result);
  return result;
}
auto difference(const cv::Mat& a, const cv::Mat& b) -> double {
  cv::Mat delta;
  cv::absdiff(a, b, delta);
  return cv::mean(delta)[0];
}
auto pause(Navigator& nav, double duration) -> std::expected<void, std::string> {
  auto deadline = Clock::now() + std::chrono::duration<double>(duration);
  while (Clock::now() < deadline) {
    if (auto ok = window::check(nav.target); !ok) return ok;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return {};
}
auto park(Navigator& nav) -> std::expected<void, std::string> {
  auto p = nav.catalog->settings.park_point;
  if (auto ok = window::position_pointer(nav.target, {p[0], p[1]}); !ok) return ok;
  nav.pointer_parked = true;
  return {};
}
auto input(Navigator& nav, cv::Point point, int notches = 0)
    -> std::expected<Boundary, std::string> {
  if (auto ok = window::position_pointer(nav.target, {point.x, point.y}); !ok)
    return std::unexpected(ok.error());
  nav.pointer_parked = false;
  if (auto ok = pause(nav, 0.15); !ok) return std::unexpected(ok.error());
  auto baseline = capture::snapshot(*nav.capture);
  if (!baseline) return std::unexpected(baseline.error());
  if (!*baseline) return std::unexpected("分类输入前捕获基准帧丢失。");
  if (auto ok = log(nav, {0,
                          {},
                          notches ? "wheel" : "click",
                          {},
                          {point.x, point.y},
                          notches,
                          (*baseline)->sequence,
                          (*baseline)->source_time});
      !ok)
    return std::unexpected(ok.error());
  Boundary boundary{*baseline, Clock::now()};
  auto sent = notches ? window::wheel(nav.target, notches) : window::click(nav.target);
  if (!sent) return std::unexpected(sent.error());
  if (!notches) {
    if (auto ok = pause(nav, 0.12); !ok) return std::unexpected(ok.error());
    if (auto ok = park(nav); !ok) return std::unexpected(ok.error());
  }
  return boundary;
}
auto wait(Navigator& nav, Boundary boundary = {},
          const std::function<bool(const cv::Mat&)>& predicate = {}, bool moved = false,
          std::optional<scan::Box> motion_box = {}) -> std::expected<Frame, std::string> {
  const auto& settings = nav.catalog->settings;
  auto area = scan::rect(motion_box.value_or(settings.menu_rect));
  cv::Mat before;
  if (moved) before = gray(boundary.before->bgr(area));
  auto deadline = Clock::now() + std::chrono::duration<double>(nav.timeout);
  auto sequence = boundary.before ? boundary.before->sequence : 0;
  auto source = boundary.before ? boundary.before->source_time : -1;
  std::vector<cv::Mat> previous;
  std::optional<Clock::time_point> stable_since;
  while (Clock::now() < deadline) {
    if (auto ok = window::check(nav.target); !ok) return std::unexpected(ok.error());
    auto snapshot = capture::snapshot(*nav.capture);
    if (!snapshot) return std::unexpected(snapshot.error());
    auto frame = *snapshot;
    if (!frame || frame->sequence <= sequence || frame->source_time <= source ||
        frame->received_at < boundary.sent_at) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    sequence = frame->sequence;
    source = frame->source_time;
    nav.last_frame = frame;
    bool ready = (!moved || difference(before, gray(frame->bgr(area))) > 0.6) &&
                 (!predicate || predicate(frame->bgr));
    if (!ready) {
      stable_since.reset();
      continue;
    }
    std::vector<scan::Box> boxes{settings.menu_rect, settings.header_rect};
    if (motion_box) boxes.push_back(*motion_box);
    std::vector<cv::Mat> sample(boxes.size());
    for (std::size_t i = 0; i < boxes.size(); ++i)
      cv::GaussianBlur(gray(frame->bgr(scan::rect(boxes[i]))), sample[i], {3, 3}, 0);
    auto now = Clock::now();
    bool changed = !stable_since;
    if (!changed)
      for (std::size_t i = 0; i < sample.size(); ++i)
        changed = changed || difference(previous[i], sample[i]) > 0.7;
    if (changed) {
      previous = std::move(sample);
      stable_since = now;
    } else if (std::chrono::duration<double>(now - *stable_since).count() >= 0.35) {
      if (auto ok = log(nav, {0, {}, "ready", {}, {}, 0, sequence, source}); !ok)
        return std::unexpected(ok.error());
      return frame;
    }
  }
  return std::unexpected(nav.stage + "：等待响应或选中状态稳定超时；未追加输入。");
}
auto bounds(const Navigator& nav, const Group& group, const cv::Mat& image)
    -> std::optional<scan::Box> {
  const auto& s = nav.catalog->settings;
  if (!group.entry) return s.root_search_rect;
  auto parent = locate(image, *group.entry, s.root_search_rect, s);
  if (!parent) return {};
  int bottom;
  if (group.definition.id == "accessories") {
    auto next = locate(image, *nav.catalog->groups[2].entry, s.root_search_rect, s);
    if (!next || next->point.y - parent->point.y < 400) return {};
    bottom = next->point.y - 38;
  } else {
    bottom = parent->point.y + group.definition.viewport_bottom_offset;
    if (bottom > s.menu_rect[3]) return {};
  }
  // 物理展开区仍可能包含底部渐隐。定位、漏识别保护和滚动规划共用可读边界。
  bottom -= group.definition.bottom_fade_inset;
  if (bottom - (parent->point.y + 43) < 300) return {};
  return scan::Box{188, parent->point.y + 43, 272, bottom};
}
auto contrast(const cv::Mat& pixels, cv::Point point, bool child) -> double {
  cv::Mat image;
  gray(pixels).convertTo(image, CV_32F);
  int half = child ? 32 : 40, span = half - 12;
  auto edge = [&](cv::Rect area, int dimension) {
    cv::Mat values;
    cv::reduce(image(area), values, dimension, cv::REDUCE_AVG, CV_32F);
    auto* data = values.ptr<float>();
    double best = -std::numeric_limits<double>::infinity();
    for (int i = 4; i < 13; ++i)
      best = std::max(best, static_cast<double>(data[i] - (data[i - 4] + data[i + 4]) * 0.5f));
    return best;
  };
  return std::min({edge({point.x - span, point.y - half - 8, span * 2 + 1, 17}, 1),
                   edge({point.x - span, point.y + half - 8, span * 2 + 1, 17}, 1),
                   edge({point.x - half - 8, point.y - span, 17, span * 2 + 1}, 0),
                   edge({point.x + half - 8, point.y - span, 17, span * 2 + 1}, 0)});
}
auto selected(const Navigator& nav, const Group& group, const Icon& icon, const cv::Mat& image,
              bool root = false) -> bool {
  const auto& s = nav.catalog->settings;
  bool child = group.entry.has_value() && !root;
  auto area = child ? bounds(nav, group, image) : std::optional{s.root_search_rect};
  if (!area) return false;
  auto match = locate(image, icon, *area, s, child);
  return match && contrast(image, match->point, child) >= s.selection_contrast;
}
auto expected_point(Navigator& nav, const Group& group, const Icon& icon, const cv::Mat& image,
                    const scan::Box& area) -> std::expected<std::optional<cv::Point>, std::string> {
  const auto& entries = group.categories;
  auto found = std::ranges::find_if(
      entries, [&](const Icon& item) { return item.definition.id == icon.definition.id; });
  int index = static_cast<int>(found - entries.begin());
  std::vector<int> neighbors;
  for (int i = 0; i < static_cast<int>(entries.size()); ++i)
    if (i != index) neighbors.push_back(i);
  std::ranges::sort(neighbors, [index](int a, int b) {
    return std::pair{std::abs(a - index), a} < std::pair{std::abs(b - index), b};
  });
  for (int i : neighbors) {
    auto match = locate(image, entries[i], area, nav.catalog->settings, true);
    if (!match) continue;
    cv::Point expected{match->point.x,
                       match->point.y + (index - i) * nav.catalog->settings.child_row_step};
    if (area[1] + 37 <= expected.y && expected.y < area[3] - 37) {
      if (auto ok = utils::image::save_png(nav.output / "unmatched_category_menu.png",
                                           image(scan::rect(nav.catalog->settings.menu_rect)));
          !ok)
        return std::unexpected(ok.error());
      if (auto ok = log(nav, {0,
                              {},
                              "unmatched_readable_category",
                              icon.definition.id,
                              {expected.x, expected.y},
                              0,
                              nav.last_frame->sequence,
                              nav.last_frame->source_time});
          !ok)
        return std::unexpected(ok.error());
      return std::unexpected(icon.definition.name +
                             "应已完整进入可读区，但图标未通过匹配；未继续滚过该分类。");
    }
    return std::optional{expected};
  }
  return std::optional<cv::Point>{};
}
auto notches_for(int distance, int rate, int limit) -> int {
  return std::clamp(static_cast<int>(std::ceil(static_cast<double>(distance) / rate)), 1, limit);
}
auto seek(Navigator& nav, const Group& group, const Icon& icon, int direction = 1,
          bool root = false) -> std::expected<Match, std::string> {
  const auto& s = nav.catalog->settings;
  bool child = group.entry.has_value() && !root;
  nav.stage = "寻找" + group.definition.name + " / " + icon.definition.name;
  for (int count = 0; count <= max_menu_scrolls; ++count) {
    if (auto ok = window::check(nav.target); !ok) return std::unexpected(ok.error());
    const auto& image = nav.last_frame->bgr;
    auto area = child ? bounds(nav, group, image) : std::optional{s.root_search_rect};
    if (!area) return std::unexpected(group.definition.name + "菜单边界不明，未发送滚轮。");
    if (auto match = locate(image, icon, *area, s, child)) return *match;
    std::optional<cv::Point> expected;
    if (child) {
      auto result = expected_point(nav, group, icon, image, *area);
      if (!result) return std::unexpected(result.error());
      expected = *result;
    }
    if (count == max_menu_scrolls) break;
    cv::Point point{s.outer_scroll_point[0], s.outer_scroll_point[1]};
    int notches = s.root_seek_notches;
    if (child) {
      point = {229, ((*area)[1] + (*area)[3]) / 2};
      int limit =
          std::max(1, ((*area)[3] - (*area)[1] - s.child_row_step) / s.wheel_pixels_per_notch);
      int distance =
          expected ? direction * (expected->y - ((*area)[1] + 80)) : 6 * s.wheel_pixels_per_notch;
      if (distance <= 0) return std::unexpected(icon.definition.name + "已越过预期滚动方向。");
      notches = notches_for(distance, s.wheel_pixels_per_notch, limit);
    }
    if (auto ok = log(nav, {0,
                            {},
                            "scroll_plan",
                            icon.definition.id,
                            expected ? std::array{expected->x, expected->y} : std::array{0, 0},
                            direction * notches});
        !ok)
      return std::unexpected(ok.error());
    auto sent = input(nav, point, direction * notches);
    if (!sent) return std::unexpected(sent.error());
    auto frame = wait(nav, *sent, {}, true, *area);
    if (!frame) return std::unexpected(frame.error());
  }
  return std::unexpected(icon.definition.name + "：达到菜单滚动事务上限，未找到完整图标。");
}
}  // namespace

auto load(const std::filesystem::path& path) -> std::expected<Catalog, std::string> {
  auto data = utils::file::read(path);
  if (!data) return std::unexpected(data.error());
  auto parsed = rfl::json::read<CatalogDefinition>(*data);
  if (!parsed) return std::unexpected("分类配置解析失败：" + parsed.error().what());
  auto definition = std::move(parsed.value());
  const auto& s = definition.settings;
  // 该导航只移植已验证布局，不把其它分辨率按比例猜测成可用。
  if (s.reference_size != std::array{1440, 5760} || s.child_row_step <= 0 ||
      s.wheel_pixels_per_notch <= 0 || s.root_seek_notches < 1 || s.root_seek_notches > 273 ||
      !std::isfinite(s.match_threshold) || s.match_threshold <= 0 || s.match_threshold > 1 ||
      !std::isfinite(s.match_margin) || s.match_margin <= 0 || s.match_margin > 2 ||
      !std::isfinite(s.selection_contrast) || s.selection_contrast <= 0)
    return std::unexpected("分类导航布局或阈值无效；当前只支持1440×5760。");
  for (auto box : {s.menu_rect, s.header_rect, s.root_search_rect})
    if (box[0] < 0 || box[1] < 0 || box[0] >= box[2] || box[1] >= box[3] ||
        box[2] > s.reference_size[0] || box[3] > s.reference_size[1])
      return std::unexpected("分类导航采样框无效。");
  if (definition.groups.size() != 3 || definition.groups[0].id != "clothing" ||
      definition.groups[1].id != "accessories" || definition.groups[2].id != "makeup" ||
      definition.groups[0].entry || !definition.groups[1].entry || !definition.groups[2].entry ||
      definition.groups[0].categories.size() < 3)
    return std::unexpected("分类分组结构无效。");
  Catalog catalog{s, {}};
  std::unordered_set<std::string> ids;
  auto make_icon = [&](IconDefinition def) -> std::expected<Icon, std::string> {
    if (def.id.empty() ||
        def.id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos ||
        !ids.insert(def.id).second || def.name.empty())
      return std::unexpected("分类标识重复或无效：" + def.id);
    auto pixels = utils::image::load(path.parent_path() / utils::string::wide(def.image));
    if (!pixels) return std::unexpected(pixels.error());
    int width = def.crop[2] - def.crop[0], height = def.crop[3] - def.crop[1];
    cv::Point offset{def.source_point[0] - def.crop[0], def.source_point[1] - def.crop[1]};
    if (pixels->cols != width || pixels->rows != height || width < 1 || height < 1 ||
        offset.x < 0 || offset.y < 0 || offset.x >= width || offset.y >= height)
      return std::unexpected("分类图标尺寸或按钮中心无效：" + def.id);
    auto result = signal(*pixels);
    if (cv::norm(result) == 0) return std::unexpected("分类图标没有有效笔画：" + def.id);
    return Icon{std::move(def), std::move(result), offset};
  };
  for (auto& group : definition.groups) {
    if (group.categories.empty() || group.bottom_fade_inset < 0 ||
        group.bottom_fade_inset > s.child_row_step)
      return std::unexpected("分类可读区的底部渐隐留白无效。");
    Group runtime{group, {}, {}};
    if (group.entry) {
      auto icon = make_icon(*group.entry);
      if (!icon) return std::unexpected(icon.error());
      runtime.entry = std::move(*icon);
    }
    for (auto& def : group.categories) {
      auto icon = make_icon(def);
      if (!icon) return std::unexpected(icon.error());
      runtime.categories.push_back(std::move(*icon));
    }
    catalog.groups.push_back(std::move(runtime));
  }
  return catalog;
}

auto locate(const cv::Mat& image, const Icon& icon, const scan::Box& bounds,
            const Settings& settings, bool child) -> std::optional<Match> {
  auto query = signal(image(scan::rect(bounds)));
  int width = icon.signal.cols, height = icon.signal.rows;
  if (query.cols < width || query.rows < height) return {};
  cv::Mat response;
  cv::matchTemplate(query, icon.signal, response, cv::TM_CCOEFF_NORMED);
  // 无纹理位置不参与峰值；与Python原型的nan_to_num语义一致。
  for (int y = 0; y < response.rows; ++y)
    for (int x = 0; x < response.cols; ++x)
      if (!std::isfinite(response.at<float>(y, x))) response.at<float>(y, x) = -1;
  double score{};
  cv::Point best;
  cv::minMaxLoc(response, nullptr, &score, nullptr, &best);
  int left = std::max(0, best.x - width / 2), top = std::max(0, best.y - height / 2);
  int right = std::min(response.cols, best.x + width / 2 + 1);
  int bottom = std::min(response.rows, best.y + height / 2 + 1);
  response(cv::Rect(left, top, right - left, bottom - top)).setTo(-1);
  double second{};
  cv::minMaxLoc(response, nullptr, &second);
  cv::Point point = cv::Point(bounds[0], bounds[1]) + best + icon.click_offset;
  const auto& vertical = child ? bounds : settings.menu_rect;
  int half = child ? 32 : 40;
  if (score < settings.match_threshold || score - second < settings.match_margin ||
      point.y - half < vertical[1] || point.y + half >= vertical[3])
    return {};
  return Match{point, score, score - second};
}

auto begin(Navigator& nav) -> std::expected<void, std::string> {
  nav.journal.open(nav.output / "navigation.jsonl", std::ios::binary);
  if (!nav.journal) return std::unexpected("无法创建分类导航日志。");
  if (auto ok = park(nav); !ok) return ok;
  if (auto frame = wait(nav); !frame) return std::unexpected(frame.error());
  const auto& s = nav.catalog->settings;
  const auto& regular = nav.catalog->groups[0].categories;
  for (int i = 0; i < 3; ++i)
    if (!locate(nav.last_frame->bgr, regular[i], s.root_search_rect, s))
      return std::unexpected("初始侧栏不符：请收起饰品/妆容分组，并将侧栏滚到顶部。");
  return {};
}

auto open_group(Navigator& nav, const Group& group) -> std::expected<void, std::string> {
  if (!group.entry) return {};
  auto match = seek(nav, group, *group.entry, 1, true);
  if (!match) return std::unexpected(match.error());
  nav.stage = "展开" + group.definition.name;
  auto sent = input(nav, match->point);
  if (!sent) return std::unexpected(sent.error());
  auto frame = wait(nav, *sent, [&](const cv::Mat& image) {
    return selected(nav, group, *group.entry, image, true);
  });
  if (!frame) return std::unexpected(frame.error());
  const auto& s = nav.catalog->settings;
  for (int count = 0; !bounds(nav, group, nav.last_frame->bgr); ++count) {
    if (count == max_menu_scrolls) return std::unexpected("露出子列表达到滚动上限。");
    auto parent = locate(nav.last_frame->bgr, *group.entry, s.root_search_rect, s);
    if (!parent) return std::unexpected(group.definition.name + "父入口丢失，未发送滚轮。");
    int distance = parent->point.y - group.definition.expanded_parent_y;
    if (distance <= 0) return std::unexpected("父入口已到目标位置，但子列表边界未确认。");
    nav.stage = "露出" + group.definition.name + "子列表（滚动外层菜单）";
    int notches = notches_for(distance, s.wheel_pixels_per_notch, 24);
    auto boundary = input(nav, {s.outer_scroll_point[0], s.outer_scroll_point[1]}, notches);
    if (!boundary) return std::unexpected(boundary.error());
    auto moved = wait(nav, *boundary, {}, true);
    if (!moved) return std::unexpected(moved.error());
  }
  auto area = *bounds(nav, group, nav.last_frame->bgr);
  if (!std::ranges::any_of(group.categories, [&](const Icon& icon) {
        return locate(nav.last_frame->bgr, icon, area, s, true).has_value();
      }))
    return std::unexpected("展开区没有找到任何子类图标。");
  // 只定位首图标以重置记忆位置；即便首类被跳过也不点击它。
  auto first = seek(nav, group, group.categories.front(), -1);
  if (!first) return std::unexpected(first.error());
  return {};
}

auto visit(Navigator& nav, const Group& group, const Icon& icon, std::string_view prefix)
    -> std::expected<void, std::string> {
  if (!icon.definition.skip_reason.empty()) return std::unexpected("不能点击跳过的分类。");
  auto match = seek(nav, group, icon);
  if (!match) return std::unexpected(match.error());
  nav.stage = "选择" + group.definition.name + " / " + icon.definition.name;
  if (!nav.pointer_parked || !selected(nav, group, icon, nav.last_frame->bgr)) {
    auto sent = input(nav, match->point);
    if (!sent) return std::unexpected(sent.error());
    auto frame =
        wait(nav, *sent, [&](const cv::Mat& image) { return selected(nav, group, icon, image); });
    if (!frame) return std::unexpected(frame.error());
  }
  const auto& s = nav.catalog->settings;
  for (auto [box, suffix] :
       {std::pair{s.menu_rect, "_menu.png"}, std::pair{s.header_rect, "_title.png"}})
    if (auto ok = utils::image::save_png(nav.output / (std::string(prefix) + suffix),
                                         nav.last_frame->bgr(scan::rect(box)));
        !ok)
      return ok;
  auto area = bounds(nav, group, nav.last_frame->bgr);
  auto actual =
      area ? locate(nav.last_frame->bgr, icon, *area, s, group.entry.has_value()) : std::nullopt;
  if (!actual) return std::unexpected("选中确认后目标图标丢失。");
  return log(nav, {0,
                   {},
                   "visited",
                   icon.definition.id,
                   {actual->point.x, actual->point.y},
                   0,
                   nav.last_frame->sequence,
                   nav.last_frame->source_time,
                   actual->score,
                   actual->margin});
}

auto reset_items_top(Navigator& nav, const scan::Layout& layout,
                     std::optional<POINT> explicit_point) -> std::expected<void, std::string> {
  nav.stage = "确认新分类物品列表回顶";
  auto fresh = wait(nav, {nav.last_frame, Clock::now()});
  if (!fresh) return std::unexpected(fresh.error());
  if (scan::items_at_top(nav.last_frame->bgr, layout)) return {};
  auto point = explicit_point ? std::expected<POINT, std::string>{*explicit_point}
                              : window::scroll_point(nav.target, layout.scroll_rect);
  if (!point) return std::unexpected(point.error());
  // 只观察滚动条的稳定，避免物品悬停特效干扰回顶事务。
  scan::Box strip = layout.bottom_detection.line_rect;
  strip[1] = layout.safe_y[0];
  for (int count = 0; count < max_menu_scrolls; ++count) {
    nav.stage = "新分类物品列表向上复位";
    auto boundary = input(nav, {point->x, point->y}, -273);
    if (!boundary) return std::unexpected(boundary.error());
    auto moved = wait(nav, *boundary, {}, true, strip);
    if (!moved) return std::unexpected(moved.error());
    if (scan::items_at_top(nav.last_frame->bgr, layout)) return {};
  }
  return std::unexpected("新分类物品列表回顶超过滚动事务上限。");
}
}  // namespace features::navigation

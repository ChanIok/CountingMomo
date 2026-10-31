#include "features/navigation/navigation.hpp"

#include "vendor/std.hpp"

#include "vendor/rfl.hpp"

#include "core/message/message.hpp"

#include "features/scan/layout.hpp"
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
  core::Message stage;
  std::string action;
  std::string target;
  std::array<int, 2> point{};
  int notches{};
  std::uint64_t sequence{};
  std::int64_t source_time{};
  double score{};
  double margin{};
};

auto log(Navigator& nav, Event event) -> std::expected<void, core::Error> {
  event.at = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
  event.stage = nav.stage;
  nav.journal << rfl::json::write(event) << '\n';
  nav.journal.flush();
  if (!nav.journal) {
    return std::unexpected(core::error("error.navigation.log_write_failed"));
  }
  return {};
}

auto gray(const cv::Mat& image) -> cv::Mat {
  cv::Mat result;
  cv::cvtColor(image, result, cv::COLOR_BGR2GRAY);
  return result;
}

auto icon_signal(const cv::Mat& image) -> cv::Mat {
  cv::Mat value;
  cv::Mat blurred;
  cv::Mat result;
  gray(image).convertTo(value, CV_32F);
  cv::GaussianBlur(value, blurred, {0, 0}, 3);
  cv::subtract(value, blurred, result);
  cv::max(result, 0, result);
  return result;
}

auto difference(const cv::Mat& first, const cv::Mat& second) -> double {
  cv::Mat delta;
  cv::absdiff(first, second, delta);
  return cv::mean(delta)[0];
}

auto pause(core::AppState& state, Navigator& nav, double duration)
    -> std::expected<void, core::Error> {
  auto deadline = Clock::now() + std::chrono::duration<double>(duration);
  while (Clock::now() < deadline) {
    if (auto ok = window::check(state, nav.target); !ok) {
      return ok;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return {};
}

auto park(core::AppState& state, Navigator& nav) -> std::expected<void, core::Error> {
  auto point = nav.catalog->settings.park_point;
  if (auto ok = window::position_pointer(state, nav.target, {point[0], point[1]}); !ok) {
    return ok;
  }
  nav.pointer_parked = true;
  return {};
}

auto send_input(core::AppState& state, Navigator& nav, cv::Point point, int notches = 0)
    -> std::expected<Boundary, core::Error> {
  if (auto ok = window::position_pointer(state, nav.target, {point.x, point.y}); !ok) {
    return std::unexpected(ok.error());
  }
  nav.pointer_parked = false;
  if (auto ok = pause(state, nav, 0.15); !ok) {
    return std::unexpected(ok.error());
  }
  auto baseline = capture::snapshot(*nav.capture);
  if (!baseline) {
    return std::unexpected(baseline.error());
  }
  if (!*baseline) {
    return std::unexpected(core::error("error.navigation.baseline_lost"));
  }
  Event event{
      .action = notches ? "wheel" : "click",
      .point = {point.x, point.y},
      .notches = notches,
      .sequence = (*baseline)->sequence,
      .source_time = (*baseline)->source_time,
  };
  if (auto ok = log(nav, std::move(event)); !ok) {
    return std::unexpected(ok.error());
  }
  Boundary boundary{*baseline, Clock::now()};
  auto sent =
      notches ? window::wheel(state, nav.target, notches) : window::click(state, nav.target);
  if (!sent) {
    return std::unexpected(sent.error());
  }
  if (!notches) {
    if (auto ok = pause(state, nav, 0.12); !ok) {
      return std::unexpected(ok.error());
    }
    if (auto ok = park(state, nav); !ok) {
      return std::unexpected(ok.error());
    }
  }
  return boundary;
}

auto wait_navigation(core::AppState& state, Navigator& nav, Boundary boundary = {},
                     const std::function<bool(const cv::Mat&)>& predicate = {}, bool moved = false,
                     std::optional<scan::Box> motion_box = {})
    -> std::expected<Frame, core::Error> {
  const auto& settings = nav.catalog->settings;
  auto area = scan::rect(motion_box.value_or(settings.menu_rect));
  cv::Mat before;
  if (moved) {
    before = gray(boundary.before->bgr(area));
  }
  auto deadline = Clock::now() + std::chrono::duration<double>(nav.timeout);
  auto sequence = boundary.before ? boundary.before->sequence : 0;
  auto source = boundary.before ? boundary.before->source_time : -1;
  std::vector<cv::Mat> previous;
  std::optional<Clock::time_point> stable_since;
  while (Clock::now() < deadline) {
    if (auto ok = window::check(state, nav.target); !ok) {
      return std::unexpected(ok.error());
    }
    auto snapshot = capture::snapshot(*nav.capture);
    if (!snapshot) {
      return std::unexpected(snapshot.error());
    }
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
    if (motion_box) {
      boxes.push_back(*motion_box);
    }
    std::vector<cv::Mat> sample(boxes.size());
    for (std::size_t i = 0; i < boxes.size(); ++i) {
      cv::GaussianBlur(gray(frame->bgr(scan::rect(boxes[i]))), sample[i], {3, 3}, 0);
    }
    auto now = Clock::now();
    bool changed = !stable_since;
    if (!changed) {
      for (std::size_t i = 0; i < sample.size(); ++i) {
        changed = changed || difference(previous[i], sample[i]) > 0.7;
      }
    }
    if (changed) {
      previous = std::move(sample);
      stable_since = now;
    } else if (std::chrono::duration<double>(now - *stable_since).count() >= 0.35) {
      if (auto ok = log(nav, {.action = "ready", .sequence = sequence, .source_time = source});
          !ok) {
        return std::unexpected(ok.error());
      }
      return frame;
    }
  }
  return std::unexpected(
      core::Error{core::message("error.navigation.timeout"), {}, {{nav.stage, {}, {}}}});
}

auto bounds(const Navigator& nav, const Group& group, const cv::Mat& image)
    -> std::optional<scan::Box> {
  const auto& settings = nav.catalog->settings;
  if (!group.entry) {
    return settings.root_search_rect;
  }
  auto parent = locate(image, *group.entry, settings.root_search_rect, settings);
  if (!parent) {
    return {};
  }
  int bottom;
  if (group.definition.id == "accessories") {
    auto next = locate(image, *nav.catalog->groups[2].entry, settings.root_search_rect, settings);
    if (!next || next->point.y - parent->point.y < 400) {
      return {};
    }
    bottom = next->point.y - 38;
  } else {
    bottom = parent->point.y + group.definition.viewport_bottom_offset;
    if (bottom > settings.menu_rect[3]) {
      return {};
    }
  }
  // 物理展开区仍可能包含底部渐隐。定位、漏识别保护和滚动规划共用可读边界。
  bottom -= group.definition.bottom_fade_inset;
  if (bottom - (parent->point.y + 43) < 300) {
    return {};
  }
  return scan::Box{188, parent->point.y + 43, 272, bottom};
}

auto contrast(const cv::Mat& pixels, cv::Point point, bool child) -> double {
  cv::Mat image;
  gray(pixels).convertTo(image, CV_32F);
  int half = child ? 32 : 40;
  int span = half - 12;
  auto edge = [&](cv::Rect area, int dimension) {
    cv::Mat values;
    cv::reduce(image(area), values, dimension, cv::REDUCE_AVG, CV_32F);
    auto* data = values.ptr<float>();
    double best = -std::numeric_limits<double>::infinity();
    for (int i = 4; i < 13; ++i) {
      best = std::max(best, static_cast<double>(data[i] - (data[i - 4] + data[i + 4]) * 0.5f));
    }
    return best;
  };
  return std::min({edge({point.x - span, point.y - half - 8, span * 2 + 1, 17}, 1),
                   edge({point.x - span, point.y + half - 8, span * 2 + 1, 17}, 1),
                   edge({point.x - half - 8, point.y - span, 17, span * 2 + 1}, 0),
                   edge({point.x + half - 8, point.y - span, 17, span * 2 + 1}, 0)});
}

auto selected(const Navigator& nav, const Group& group, const Icon& icon, const cv::Mat& image,
              bool root = false) -> bool {
  const auto& settings = nav.catalog->settings;
  bool child = group.entry.has_value() && !root;
  auto area = child ? bounds(nav, group, image) : std::optional{settings.root_search_rect};
  if (!area) {
    return false;
  }
  auto match = locate(image, icon, *area, settings, child);
  return match && contrast(image, match->point, child) >= settings.selection_contrast;
}

auto expected_point(Navigator& nav, const Group& group, const Icon& icon, const cv::Mat& image,
                    const scan::Box& area) -> std::expected<std::optional<cv::Point>, core::Error> {
  const auto& entries = group.categories;
  auto found = std::ranges::find_if(
      entries, [&](const Icon& item) { return item.definition.id == icon.definition.id; });
  int index = static_cast<int>(found - entries.begin());
  std::vector<int> neighbors;
  for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
    if (i != index) {
      neighbors.push_back(i);
    }
  }
  std::ranges::sort(neighbors, [index](int a, int b) {
    return std::pair{std::abs(a - index), a} < std::pair{std::abs(b - index), b};
  });
  for (int i : neighbors) {
    auto match = locate(image, entries[i], area, nav.catalog->settings, true);
    if (!match) {
      continue;
    }
    cv::Point expected{match->point.x,
                       match->point.y + (index - i) * nav.catalog->settings.child_row_step};
    if (area[1] + 37 <= expected.y && expected.y < area[3] - 37) {
      if (auto ok = utils::image::save_png(nav.output / "unmatched_category_menu.png",
                                           image(scan::rect(nav.catalog->settings.menu_rect)));
          !ok) {
        return std::unexpected(ok.error());
      }
      Event event{
          .action = "unmatched_readable_category",
          .target = icon.definition.id,
          .point = {expected.x, expected.y},
          .sequence = nav.last_frame->sequence,
          .source_time = nav.last_frame->source_time,
      };
      if (auto ok = log(nav, std::move(event)); !ok) {
        return std::unexpected(ok.error());
      }
      return std::unexpected(core::error("error.navigation.unmatched_readable",
                                         core::text_key("category." + icon.definition.id)));
    }
    return std::optional{expected};
  }
  return std::optional<cv::Point>{};
}

auto notches_for(int distance, int rate, int limit) -> int {
  return std::clamp(static_cast<int>(std::ceil(static_cast<double>(distance) / rate)), 1, limit);
}

auto seek(core::AppState& state, Navigator& nav, const Group& group, const Icon& icon,
          int direction = 1, bool root = false) -> std::expected<Match, core::Error> {
  const auto& settings = nav.catalog->settings;
  bool child = group.entry.has_value() && !root;
  nav.stage =
      core::message("navigation.stage.seeking", core::text_key("group." + group.definition.id),
                    core::text_key((root ? "group." : "category.") + icon.definition.id));
  for (int count = 0; count <= max_menu_scrolls; ++count) {
    if (auto ok = window::check(state, nav.target); !ok) {
      return std::unexpected(ok.error());
    }
    const auto& image = nav.last_frame->bgr;
    auto area = child ? bounds(nav, group, image) : std::optional{settings.root_search_rect};
    if (!area) {
      return std::unexpected(core::error("error.navigation.bounds_unknown",
                                         core::text_key("group." + group.definition.id)));
    }
    if (auto match = locate(image, icon, *area, settings, child)) {
      return *match;
    }
    std::optional<cv::Point> expected;
    if (child) {
      auto result = expected_point(nav, group, icon, image, *area);
      if (!result) {
        return std::unexpected(result.error());
      }
      expected = *result;
    }
    if (count == max_menu_scrolls) {
      break;
    }
    cv::Point point{settings.outer_scroll_point[0], settings.outer_scroll_point[1]};
    int notches = settings.root_seek_notches;
    if (child) {
      point = {229, ((*area)[1] + (*area)[3]) / 2};
      int limit = std::max(
          1, ((*area)[3] - (*area)[1] - settings.child_row_step) / settings.wheel_pixels_per_notch);
      int distance = expected ? direction * (expected->y - ((*area)[1] + 80))
                              : 6 * settings.wheel_pixels_per_notch;
      if (distance <= 0) {
        return std::unexpected(core::error("error.navigation.wrong_direction",
                                           core::text_key("category." + icon.definition.id)));
      }
      notches = notches_for(distance, settings.wheel_pixels_per_notch, limit);
    }
    Event event{
        .action = "scroll_plan",
        .target = icon.definition.id,
        .point = expected ? std::array{expected->x, expected->y} : std::array{0, 0},
        .notches = direction * notches,
    };
    if (auto ok = log(nav, std::move(event)); !ok) {
      return std::unexpected(ok.error());
    }
    auto sent = send_input(state, nav, point, direction * notches);
    if (!sent) {
      return std::unexpected(sent.error());
    }
    auto frame = wait_navigation(state, nav, *sent, {}, true, *area);
    if (!frame) {
      return std::unexpected(frame.error());
    }
  }
  return std::unexpected(
      core::error("error.navigation.seek_limit",
                  core::text_key((root ? "group." : "category.") + icon.definition.id)));
}

}  // namespace

auto load(const std::filesystem::path& path) -> std::expected<Catalog, core::Error> {
  auto data = utils::file::read(path);
  if (!data) {
    return std::unexpected(data.error());
  }
  auto parsed = rfl::json::read<CatalogDefinition>(*data);
  if (!parsed) {
    return std::unexpected(
        core::with_detail(core::error("error.navigation.parse_failed"), parsed.error().what()));
  }
  auto definition = std::move(parsed.value());
  const auto& settings = definition.settings;
  // 图标按 1440×5760 原生像素标定，不按比例推测其它分辨率。
  if (settings.reference_size != std::array{1440, 5760} || settings.child_row_step <= 0 ||
      settings.wheel_pixels_per_notch <= 0 || settings.root_seek_notches < 1 ||
      settings.root_seek_notches > 273 || !std::isfinite(settings.match_threshold) ||
      settings.match_threshold <= 0 || settings.match_threshold > 1 ||
      !std::isfinite(settings.match_margin) || settings.match_margin <= 0 ||
      settings.match_margin > 2 || !std::isfinite(settings.selection_contrast) ||
      settings.selection_contrast <= 0) {
    return std::unexpected(core::error("error.navigation.invalid_layout"));
  }
  for (auto box : {settings.menu_rect, settings.header_rect, settings.root_search_rect}) {
    if (box[0] < 0 || box[1] < 0 || box[0] >= box[2] || box[1] >= box[3] ||
        box[2] > settings.reference_size[0] || box[3] > settings.reference_size[1]) {
      return std::unexpected(core::error("error.navigation.invalid_sample_box"));
    }
  }
  if (definition.groups.size() != 3 || definition.groups[0].id != "clothing" ||
      definition.groups[1].id != "accessories" || definition.groups[2].id != "makeup" ||
      definition.groups[0].entry || !definition.groups[1].entry || !definition.groups[2].entry ||
      definition.groups[0].categories.size() < 3) {
    return std::unexpected(core::error("error.navigation.invalid_groups"));
  }
  Catalog catalog{settings, {}};
  std::unordered_set<std::string> ids;
  auto make_icon = [&](IconDefinition definition) -> std::expected<Icon, core::Error> {
    if (definition.id.empty() ||
        definition.id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") !=
            std::string::npos ||
        !ids.insert(definition.id).second) {
      return std::unexpected(core::error("error.navigation.invalid_id", definition.id));
    }
    auto pixels = utils::image::load(path.parent_path() / utils::string::wide(definition.image));
    if (!pixels) {
      return std::unexpected(pixels.error());
    }
    int width = definition.crop[2] - definition.crop[0];
    int height = definition.crop[3] - definition.crop[1];
    cv::Point offset{definition.source_point[0] - definition.crop[0],
                     definition.source_point[1] - definition.crop[1]};
    if (pixels->cols != width || pixels->rows != height || width < 1 || height < 1 ||
        offset.x < 0 || offset.y < 0 || offset.x >= width || offset.y >= height) {
      return std::unexpected(core::error("error.navigation.invalid_icon_geometry", definition.id));
    }
    auto result = icon_signal(*pixels);
    if (cv::norm(result) == 0) {
      return std::unexpected(core::error("error.navigation.empty_icon", definition.id));
    }
    return Icon{std::move(definition), std::move(result), offset};
  };
  for (auto& group : definition.groups) {
    if (group.categories.empty() || group.bottom_fade_inset < 0 ||
        group.bottom_fade_inset > settings.child_row_step) {
      return std::unexpected(core::error("error.navigation.invalid_fade_inset"));
    }
    Group runtime{group, {}, {}};
    if (group.entry) {
      auto icon = make_icon(*group.entry);
      if (!icon) {
        return std::unexpected(icon.error());
      }
      runtime.entry = std::move(*icon);
    }
    for (auto& definition : group.categories) {
      auto icon = make_icon(definition);
      if (!icon) {
        return std::unexpected(icon.error());
      }
      runtime.categories.push_back(std::move(*icon));
    }
    catalog.groups.push_back(std::move(runtime));
  }
  return catalog;
}

auto locate(const cv::Mat& image, const Icon& icon, const scan::Box& bounds,
            const Settings& settings, bool child) -> std::optional<Match> {
  auto query = icon_signal(image(scan::rect(bounds)));
  int width = icon.signal.cols;
  int height = icon.signal.rows;
  if (query.cols < width || query.rows < height) {
    return {};
  }
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
  int left = std::max(0, best.x - width / 2);
  int top = std::max(0, best.y - height / 2);
  int right = std::min(response.cols, best.x + width / 2 + 1);
  int bottom = std::min(response.rows, best.y + height / 2 + 1);
  response(cv::Rect(left, top, right - left, bottom - top)).setTo(-1);
  double second{};
  cv::minMaxLoc(response, nullptr, &second);
  cv::Point point = cv::Point(bounds[0], bounds[1]) + best + icon.click_offset;
  const auto& vertical = child ? bounds : settings.menu_rect;
  int half = child ? 32 : 40;
  if (score < settings.match_threshold || score - second < settings.match_margin ||
      point.y - half < vertical[1] || point.y + half >= vertical[3]) {
    return {};
  }
  return Match{point, score, score - second};
}

auto begin(core::AppState& state, Navigator& nav) -> std::expected<void, core::Error> {
  nav.journal.open(nav.output / "navigation.jsonl", std::ios::binary);
  if (!nav.journal) {
    return std::unexpected(core::error("error.navigation.log_open_failed"));
  }
  if (auto ok = park(state, nav); !ok) {
    return ok;
  }
  const auto& settings = nav.catalog->settings;
  const auto& regular = nav.catalog->groups[0].categories;
  nav.stage = core::message("navigation.stage.waiting_initial");
  auto frame = wait_navigation(state, nav, {}, [&](const cv::Mat& image) {
    for (int i = 0; i < 3; ++i) {
      if (!locate(image, regular[i], settings.root_search_rect, settings)) {
        return false;
      }
    }
    return true;
  });
  if (!frame) {
    if (auto ok = window::check(state, nav.target); !ok) {
      return ok;
    }
    return std::unexpected(core::error("error.navigation.initial_not_found"));
  }
  return {};
}

auto open_group(core::AppState& state, Navigator& nav, const Group& group)
    -> std::expected<void, core::Error> {
  if (!group.entry) {
    return {};
  }
  auto match = seek(state, nav, group, *group.entry, 1, true);
  if (!match) {
    return std::unexpected(match.error());
  }
  nav.stage =
      core::message("navigation.stage.opening", core::text_key("group." + group.definition.id));
  auto sent = send_input(state, nav, match->point);
  if (!sent) {
    return std::unexpected(sent.error());
  }
  auto frame = wait_navigation(state, nav, *sent, [&](const cv::Mat& image) {
    return selected(nav, group, *group.entry, image, true);
  });
  if (!frame) {
    return std::unexpected(frame.error());
  }
  const auto& settings = nav.catalog->settings;
  for (int count = 0; !bounds(nav, group, nav.last_frame->bgr); ++count) {
    if (count == max_menu_scrolls) {
      return std::unexpected(core::error("error.navigation.reveal_limit"));
    }
    auto parent = locate(nav.last_frame->bgr, *group.entry, settings.root_search_rect, settings);
    if (!parent) {
      return std::unexpected(core::error("error.navigation.parent_lost",
                                         core::text_key("group." + group.definition.id)));
    }
    int distance = parent->point.y - group.definition.expanded_parent_y;
    if (distance <= 0) {
      return std::unexpected(core::error("error.navigation.child_bounds_unknown"));
    }
    nav.stage =
        core::message("navigation.stage.revealing", core::text_key("group." + group.definition.id));
    int notches = notches_for(distance, settings.wheel_pixels_per_notch, 24);
    auto boundary = send_input(
        state, nav, {settings.outer_scroll_point[0], settings.outer_scroll_point[1]}, notches);
    if (!boundary) {
      return std::unexpected(boundary.error());
    }
    auto moved = wait_navigation(state, nav, *boundary, {}, true);
    if (!moved) {
      return std::unexpected(moved.error());
    }
  }
  auto area = *bounds(nav, group, nav.last_frame->bgr);
  if (!std::ranges::any_of(group.categories, [&](const Icon& icon) {
        return locate(nav.last_frame->bgr, icon, area, settings, true).has_value();
      })) {
    return std::unexpected(core::error("error.navigation.no_child_icons"));
  }
  // 将首个目标分类滚到可读区，不在定位阶段点击。
  auto first = seek(state, nav, group, group.categories.front(), -1);
  if (!first) {
    return std::unexpected(first.error());
  }
  return {};
}

auto visit(core::AppState& state, Navigator& nav, const Group& group, const Icon& icon,
           std::string_view prefix) -> std::expected<void, core::Error> {
  auto match = seek(state, nav, group, icon);
  if (!match) {
    return std::unexpected(match.error());
  }
  nav.stage =
      core::message("navigation.stage.selecting", core::text_key("group." + group.definition.id),
                    core::text_key("category." + icon.definition.id));
  if (!nav.pointer_parked) {
    auto before_park = nav.last_frame;
    if (auto parked = park(state, nav); !parked) {
      return parked;
    }
    // 悬停可能影响选中外观；先确认停放后的新帧稳定，不以目标已选中作为等待条件。
    auto frame =
        wait_navigation(state, nav, {.before = std::move(before_park), .sent_at = Clock::now()});
    if (!frame) {
      return std::unexpected(frame.error());
    }
    auto area = bounds(nav, group, nav.last_frame->bgr);
    auto refreshed = area ? locate(nav.last_frame->bgr, icon, *area, nav.catalog->settings,
                                   group.entry.has_value())
                          : std::nullopt;
    if (!refreshed) {
      return std::unexpected(core::error("error.navigation.target_lost_after_park"));
    }
    *match = *refreshed;
  }
  if (!selected(nav, group, icon, nav.last_frame->bgr)) {
    auto sent = send_input(state, nav, match->point);
    if (!sent) {
      return std::unexpected(sent.error());
    }
    auto frame = wait_navigation(
        state, nav, *sent, [&](const cv::Mat& image) { return selected(nav, group, icon, image); });
    if (!frame) {
      return std::unexpected(frame.error());
    }
  }
  const auto& settings = nav.catalog->settings;
  for (auto [box, suffix] : {std::pair{settings.menu_rect, "_menu.png"},
                             std::pair{settings.header_rect, "_title.png"}}) {
    if (auto ok = utils::image::save_png(nav.output / (std::string(prefix) + suffix),
                                         nav.last_frame->bgr(scan::rect(box)));
        !ok) {
      return ok;
    }
  }
  auto area = bounds(nav, group, nav.last_frame->bgr);
  auto actual = area ? locate(nav.last_frame->bgr, icon, *area, settings, group.entry.has_value())
                     : std::nullopt;
  if (!actual) {
    return std::unexpected(core::error("error.navigation.target_lost_after_selection"));
  }
  Event event{
      .action = "visited",
      .target = icon.definition.id,
      .point = {actual->point.x, actual->point.y},
      .sequence = nav.last_frame->sequence,
      .source_time = nav.last_frame->source_time,
      .score = actual->score,
      .margin = actual->margin,
  };
  return log(nav, std::move(event));
}

auto reset_items_top(core::AppState& state, Navigator& nav, const scan::Layout& layout)
    -> std::expected<void, core::Error> {
  nav.stage = core::message("navigation.stage.checking_top");
  auto fresh = wait_navigation(state, nav, {nav.last_frame, Clock::now()});
  if (!fresh) {
    return std::unexpected(fresh.error());
  }
  if (scan::items_at_top(nav.last_frame->bgr, layout)) {
    return {};
  }
  auto point = window::scroll_point(nav.target, layout.scroll_rect);
  if (!point) {
    return std::unexpected(point.error());
  }
  // 只观察滚动条的稳定，避免物品悬停特效干扰回顶事务。
  scan::Box strip = layout.bottom_detection.line_rect;
  strip[1] = layout.safe_y[0];
  for (int count = 0; count < max_menu_scrolls; ++count) {
    nav.stage = core::message("navigation.stage.resetting_top");
    auto boundary = send_input(state, nav, {point->x, point->y}, -273);
    if (!boundary) {
      return std::unexpected(boundary.error());
    }
    auto moved = wait_navigation(state, nav, *boundary, {}, true, strip);
    if (!moved) {
      return std::unexpected(moved.error());
    }
    if (scan::items_at_top(nav.last_frame->bgr, layout)) {
      return {};
    }
  }
  return std::unexpected(core::error("error.navigation.reset_limit"));
}

}  // namespace features::navigation

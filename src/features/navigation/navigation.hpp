#pragma once

#include "vendor/std.hpp"

#include "features/capture/capture.hpp"
#include "features/scan/types.hpp"
#include "features/window/window.hpp"

namespace features::navigation {
struct IconDefinition {
  std::string id;
  std::string name;
  std::string image;
  scan::Box crop;
  std::array<int, 2> source_point;
  std::string skip_reason;
};
struct GroupDefinition {
  std::string id;
  std::string name;
  int expanded_parent_y{};
  int viewport_bottom_offset{};
  int bottom_fade_inset{};
  std::optional<IconDefinition> entry;
  std::vector<IconDefinition> categories;
};
struct Settings {
  std::array<int, 2> reference_size;
  scan::Box menu_rect;
  scan::Box header_rect;
  scan::Box root_search_rect;
  std::array<int, 2> outer_scroll_point;
  std::array<int, 2> park_point;
  double match_threshold;
  double match_margin;
  double selection_contrast;
  int child_row_step;
  int wheel_pixels_per_notch;
  int root_seek_notches;
};
struct Icon {
  IconDefinition definition;
  cv::Mat signal;
  cv::Point click_offset;
};
struct Group {
  GroupDefinition definition;
  std::optional<Icon> entry;
  std::vector<Icon> categories;
};
struct Catalog {
  Settings settings;
  std::vector<Group> groups;
};
struct Match {
  cv::Point point;
  double score{};
  double margin{};
};
struct Navigator {
  const Catalog* catalog{};
  window::Target target;
  capture::Capture* capture{};
  std::filesystem::path output;
  double timeout{8};
  std::ofstream journal;
  std::shared_ptr<const capture::Frame> last_frame;
  std::string stage{"初始化导航"};
  bool pointer_parked{};
};

auto load(const std::filesystem::path& path) -> std::expected<Catalog, std::string>;

auto locate(const cv::Mat& image, const Icon& icon, const scan::Box& bounds,
            const Settings& settings, bool child = false) -> std::optional<Match>;

auto begin(Navigator& nav) -> std::expected<void, std::string>;

auto open_group(Navigator& nav, const Group& group) -> std::expected<void, std::string>;

// skip_reason非空的分类不允许访问；仍可作为定位相邻分类的图像锚点。
auto visit(Navigator& nav, const Group& group, const Icon& icon, std::string_view prefix)
    -> std::expected<void, std::string>;

// 先确认是否需要复位，只有实际需要输入时才定位、移动到物品滚动区。
auto reset_items_top(Navigator& nav, const scan::Layout& layout,
                     std::optional<POINT> explicit_point = {}) -> std::expected<void, std::string>;
}  // namespace features::navigation

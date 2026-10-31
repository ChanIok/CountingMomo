#pragma once

#include "vendor/std.hpp"

#include "vendor/opencv.hpp"
#include "vendor/windows.hpp"

#include "core/message/message.hpp"

#include "features/capture/capture.hpp"
#include "features/scan/types.hpp"
#include "features/window/window.hpp"

namespace features::navigation {

struct IconDefinition {
  std::string id;
  std::string image;
  scan::Box crop;                   // 素材在原始客户区截图中的裁剪区域。
  std::array<int, 2> source_point;  // 同一原始截图中的按钮中心。
};

struct GroupDefinition {
  std::string id;
  int expanded_parent_y{};
  int viewport_bottom_offset{};
  int bottom_fade_inset{};  // 从展开区底部扣除的不可读渐隐高度（px）。
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
  cv::Point click_offset;  // 相对图标素材左上角的按钮中心。
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
  double timeout{8};  // 单次导航事务超时（秒）。
  std::ofstream journal;
  std::shared_ptr<const capture::Frame> last_frame;
  core::Message stage{core::message("navigation.stage.initializing")};
  bool pointer_parked{};
};

// 读取分类清单并加载图标，素材路径相对清单文件。
auto load(const std::filesystem::path& path) -> std::expected<Catalog, core::Error>;

auto locate(const cv::Mat& image, const Icon& icon, const scan::Box& bounds,
            const Settings& settings, bool child = false) -> std::optional<Match>;

// 在现有事务超时内等待初始图标可定位、菜单和标题跨新源帧稳定。
// 只确认侧栏已回顶、分组已收起，不代替用户重置导航初始状态。
auto begin(core::AppState& state, Navigator& nav) -> std::expected<void, core::Error>;

auto open_group(core::AppState& state, Navigator& nav, const Group& group)
    -> std::expected<void, core::Error>;

// 先消除鼠标悬停影响，目标已选中则不重复点击；两条路径均确认并记录目标状态。
auto visit(core::AppState& state, Navigator& nav, const Group& group, const Icon& icon,
           std::string_view prefix) -> std::expected<void, core::Error>;

// 先确认是否需要复位，只有实际需要输入时才定位、移动到物品滚动区。
// 返回时 last_frame 是回顶确认帧，分类采集必须等待晚于它的新源帧。
auto reset_items_top(core::AppState& state, Navigator& nav, const scan::Layout& layout)
    -> std::expected<void, core::Error>;
}  // namespace features::navigation

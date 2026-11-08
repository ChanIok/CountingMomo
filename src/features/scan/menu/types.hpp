#pragma once

#include "vendor/std.hpp"

#include "vendor/opencv.hpp"

#include "core/message/types.hpp"
#include "features/scan/capture/types.hpp"

namespace features::scan::menu {

using Box = std::array<int, 4>;

// 左侧菜单外层行的固定行数：索引 0 为顶部占位，1..7 为服装分类，8/9 为分组入口
inline constexpr int root_row_count = 10;

struct IconDefinition {
  std::string_view id;
  int size;  // 图标正方形素材的像素边长
};

struct GroupDefinition {
  std::string_view id;
  int expanded_parent_y{};
  int viewport_bottom_offset{};
  int bottom_fade_inset{};  // 展开区底部的渐隐高度（px）
  std::optional<IconDefinition> entry;
  std::span<const IconDefinition> categories;
};

struct Icon {
  const IconDefinition* definition{};
  cv::Mat signal;
};

struct Group {
  const GroupDefinition* definition{};
  std::optional<Icon> entry;
  std::vector<Icon> categories;
};

struct Catalog {
  std::vector<Group> groups;
};

struct Match {
  cv::Point point;
};

// 当前帧推断出的外层菜单整列排布
struct RootLayout {
  int origin{};
  bool expanded{};      // 饰品分组已展开（妆容入口随之下移）
  int highlighted{-1};  // 当前高亮所在行索引，-1 表示未处于可靠高亮
  std::array<cv::Point, root_row_count> points{};
};

// 单次扫描任务中的菜单会话数据
struct MenuState {
  Catalog catalog{};
  double frame_timeout{};     // 画面稳定等待超时（秒）
  double pixels_per_notch{};  // 滚轮每格像素比率标定值
  std::optional<capture::Frame> last_frame;
  core::Message stage{};
  bool pointer_parked{};
};

}  // namespace features::scan::menu

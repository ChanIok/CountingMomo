#pragma once

#include "vendor/std.hpp"

#include "vendor/opencv.hpp"

namespace features::navigation {

struct IconDefinition {
  std::string_view id;
  int size;  // 正方形素材的原生像素边长，按钮中心位于素材中心。
};

struct GroupDefinition {
  std::string_view id;
  int expanded_parent_y{};
  int viewport_bottom_offset{};
  int bottom_fade_inset{};  // 从展开区底部扣除的不可读渐隐高度（px）。
  std::optional<IconDefinition> entry;
  std::span<const IconDefinition> categories;
};

struct Icon {
  const IconDefinition* definition{};  // 借用导航模块的静态定义。
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
}  // namespace features::navigation

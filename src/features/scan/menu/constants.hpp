#pragma once

#include "vendor/std.hpp"

#include "features/scan/menu/types.hpp"

namespace features::scan::menu {

// 菜单域共享的静态几何定义：视觉推断与导航编排共同使用，960 分辨率实测
inline constexpr Box menu_rect{120, 57, 186, 485};

inline constexpr Box root_search_rect{120, 63, 186, 469};

inline constexpr std::array<int, 2> outer_scroll_point{153, 84};

inline constexpr int child_row_step = 45;

inline constexpr std::array<int, 2> child_search_x{125, 181};
inline constexpr int child_top_offset = 29;
inline constexpr int child_min_height = 200;
inline constexpr int next_parent_bottom_inset = 25;

inline constexpr int root_half_size = 27;
inline constexpr int child_half_size = 21;
inline constexpr int selection_corner_inset = 8;
inline constexpr int selection_band_half = 5;
inline constexpr int selection_contrast_radius = 3;

// 左侧菜单外层行的固定排布：索引 0 是顶部无模板项，1..7 为服装分类，8/9 为分组入口
// 数值为相对 hair 中心的累计像素偏移（960 分辨率实测）
inline constexpr std::array<int, root_row_count> root_offsets{-55, 0,   55,  110, 165,
                                                              220, 276, 332, 391, 447};
inline constexpr std::array<std::string_view, root_row_count> root_ids{
    "",        "hair",  "dresses", "outerwear",   "tops",
    "bottoms", "socks", "shoes",   "accessories", "makeup"};
inline constexpr int expanded_makeup_offset = 693;  // 饰品子视口展开后，妆容入口随之后移

// 饰品子视口高度：饰品行到展开态妆容入口的间距
inline constexpr int accessories_viewport_height = expanded_makeup_offset - root_offsets[8];

// 妆容入口在展开态与折叠态下的间距中点，用于备选展开判定
inline constexpr int expanded_makeup_midpoint = (root_offsets.back() + expanded_makeup_offset) / 2;

inline constexpr int layout_anchor_tolerance = 2;

// 推算中心的取整误差补偿窗口
inline constexpr int layout_probe_radius = 3;

// 子项列表判定展开的最小视口高度（至少露出两行）
inline constexpr int child_list_probe_height =
    child_row_step + 2 * child_half_size + layout_probe_radius;

// 外层 hair 行可靠采样的最小 y 坐标
inline constexpr int root_layout_min_y =
    menu_rect[1] + root_half_size + selection_band_half + layout_probe_radius;

}  // namespace features::scan::menu

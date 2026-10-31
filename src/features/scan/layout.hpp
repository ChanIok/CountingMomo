#pragma once

#include "vendor/std.hpp"

#include "vendor/opencv.hpp"

#include "features/scan/types.hpp"

namespace features::scan {

// 原生像素布局由扫描模块固定定义，不随批次或分类修改。
auto get_layout() -> const Layout&;

// 将右下边界不包含的布局矩形转换为 OpenCV 区域，保持原坐标系，不缩放。
auto rect(const Box& box) -> cv::Rect;

}  // namespace features::scan

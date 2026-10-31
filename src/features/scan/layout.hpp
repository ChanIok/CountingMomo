#pragma once

#include "vendor/std.hpp"

#include "vendor/opencv.hpp"

#include "core/message/message.hpp"

#include "features/scan/types.hpp"

namespace features::scan {

// 将右下边界不包含的布局矩形转换为 OpenCV 区域，保持原坐标系，不缩放。
auto rect(const Box& box) -> cv::Rect;

// 读取并校验布局几何、采样区域和判定阈值；素材加载由视觉模块负责。
auto load_layout(const std::filesystem::path& path) -> std::expected<Layout, core::Error>;

}  // namespace features::scan

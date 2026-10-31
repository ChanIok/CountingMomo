#pragma once

#include "vendor/std.hpp"

#include "vendor/opencv.hpp"

#include "core/message/message.hpp"

namespace utils::image {

// 使用 WIC 解码为原生尺寸的 CV_8UC3 BGR 图像，不缩放。
auto load(const std::filesystem::path& path) -> std::expected<cv::Mat, core::Error>;

auto save_png(const std::filesystem::path& path, const cv::Mat& bgr)
    -> std::expected<void, core::Error>;
}  // namespace utils::image

#pragma once

#include "vendor/std.hpp"

#include "vendor/opencv.hpp"

namespace utils::image {
auto load(const std::filesystem::path& path) -> std::expected<cv::Mat, std::string>;

auto save_png(const std::filesystem::path& path, const cv::Mat& bgr)
    -> std::expected<void, std::string>;
// 整页证据使用固定质量100；实时识别不读取压缩后的图片。
auto save_jpg(const std::filesystem::path& path, const cv::Mat& bgr)
    -> std::expected<void, std::string>;
}  // namespace utils::image

#pragma once

#include "vendor/std.hpp"

#include "vendor/opencv.hpp"

namespace features::capture {

struct Settings {
  int width{};
  int height{};
  std::array<int, 4> comparison_rect{};  // 客户区坐标，右下边界不包含。
  double stable_seconds{};
};

struct Frame {
  std::int64_t source_time{};  // WGC 合成时刻的 QPC 时间，单位为 100ns。
  cv::Mat bgr;                 // 发布后不再修改像素；按值复制只共享 OpenCV 的像素存储。
};

}  // namespace features::capture

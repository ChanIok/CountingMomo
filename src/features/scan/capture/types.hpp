#pragma once

#include "vendor/std.hpp"

#include "vendor/opencv.hpp"

namespace features::scan::capture {

struct Settings {
  int width{};
  int height{};
  std::array<int, 4>
      comparison_rect{};    // 判稳比对区域（客户区相对坐标 [left, top, right, bottom]）
  double stable_seconds{};  // 判定画面稳定所需的持续时长（秒）
};

struct Frame {
  std::int64_t source_time{};  // 帧合成的 QPC 时间戳（100ns 单位）
  cv::Mat bgr;                 // 稳定帧的 BGR 图像数据
};

}  // namespace features::scan::capture

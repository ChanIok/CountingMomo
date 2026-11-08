#pragma once

#include "vendor/std.hpp"

#include "vendor/opencv.hpp"

#include "features/scan/capture/types.hpp"
#include "features/scan/types.hpp"

namespace features::scan::pages {

// 每格滚轮对应像素位移与单页滚动格数的动态标定结果。
struct ScrollPlan {
  int page_notches{};
  double pixels_per_notch{};
};

// 单个分类的翻页采集过程；每次切换分类整体重置。
struct CategoryState {
  std::vector<int> values;
  cv::Mat last_image;
  std::int64_t last_frame_source_time{};
  Page previous;
  int accepted_rows{};
  int previous_start{};
};

// 页面扫描会话：跨分类复用的资源、滚轮标定与当前分类采集状态。
struct PagesState {
  cv::Mat hidden_question;
  ScrollPlan scroll;
  // 本分类切换后的完整稳定帧，直接作为首屏候选；未就绪才等待变化后重采样。
  std::optional<capture::Frame> category_frame;
  CategoryState category;
};

}  // namespace features::scan::pages

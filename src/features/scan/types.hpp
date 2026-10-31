#pragma once

#include "vendor/std.hpp"

#include "vendor/opencv.hpp"

namespace features::scan {

// [left, top, right, bottom]，右下边界不包含；单位为原生像素。
using Box = std::array<int, 4>;

struct Layout {
  std::array<int, 2> reference_size;
  Box stability_rect;  // 排除最左侧主菜单光效，保留分类栏与物品列表。
  std::array<int, 3> columns;
  std::array<int, 3> columns_without_scrollbar;
  int first_row_y;
  int row_step;
  std::array<int, 2> card_size;
  std::array<int, 2> safe_y;
  int scrolled_safe_top;
  int bottom_safe_y;
  Box scroll_rect;         // 相对客户区左上角，只有需要滚动时才解析实际输入点。
  Box name_tracking_rect;  // 相对卡片左上角的文字追踪区域，不识别名称。
};

struct Sample {
  int column{};  // 从 1 开始的列号，问号卡同样占用位置。
  int owned{};   // 业务值只有 0 / 1；未就绪的采样不作为页面结果接受。
  bool hidden{};
  double star_luma{};
  cv::Mat name;  // 文字灰度裁图，只用于接续，不识别名称。
};

struct Row {
  int y{};
  std::vector<Sample> cards;
};

struct BottomResult {
  bool at_bottom{};
  double clear_fraction{};
  double contrast_median{};
};

struct Page {
  cv::Mat image;
  cv::Mat gray;
  std::vector<Row> rows;
  int phase{};  // 行首 y 对行距取余，范围为 [0, row_step)。
  BottomResult bottom;
  bool single_page{};
};

struct AnchorTemplate {
  int row{};     // 相对锚点起始行，从 0 开始。
  int column{};  // 布局列下标，从 0 开始。
  cv::Mat pixels;
};

struct Anchor {
  int y{};
  int span_rows{};
  std::vector<AnchorTemplate> templates;
};

struct AnchorMatch {
  bool valid{};
  int displacement{};  // 原锚点 y 减去当前 y（px）；向下滚动列表时为正。
};

}  // namespace features::scan

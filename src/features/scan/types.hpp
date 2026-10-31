#pragma once

#include "vendor/std.hpp"

#include "vendor/opencv.hpp"

#include "core/message/types.hpp"
#include "features/scan/stage.hpp"

namespace features::scan {

// [left, top, right, bottom]，右下边界不包含；单位为原生像素。
using Box = std::array<int, 4>;

// 普通卡边框的两条采样带，坐标相对卡片左上角。
struct CardFrame {
  Box bottom_rect;
  Box right_rect;
  double min_contrast;
};

// 滚动条采样带及读取下边界，坐标相对客户区左上角。
struct BottomSettings {
  Box line_rect;
  Box left_rect;
  Box right_rect;
  double min_contrast;
  double min_clear_fraction;
  int safe_bottom;
};

struct OwnershipSettings {
  std::vector<std::string> foreground_mask;
  double owned_luma;
  double unowned_luma;
  double max_luma_error;
};

struct HiddenCardSettings {
  std::string image;
  Box search_rect;  // 相对卡片左上角的问号搜索区域。
  double min_score;
};

struct Layout {
  std::array<int, 2> reference_size;
  std::vector<int> columns;
  std::vector<int> columns_without_scrollbar;
  int first_row_y;
  int row_step;
  std::array<int, 2> card_size;
  CardFrame card_frame;
  std::array<int, 2> safe_y;
  int scrolled_safe_top;
  Box scroll_rect;  // 相对客户区左上角，只有需要滚动时才解析实际输入点。
  BottomSettings bottom_detection;
  Box name_tracking_rect;  // 相对卡片左上角的文字追踪区域。
  Box star_rect;           // 相对卡片左上角的最右侧星星区域。
  OwnershipSettings ownership_star;
  HiddenCardSettings hidden_card;
};

struct Sample {
  int column{};  // 从 1 开始的列号，问号卡同样占用位置。
  int owned{};   // 业务值只有 0 / 1；未就绪的采样不作为页面结果接受。
  bool hidden{};
  double star_luma{};
  cv::Mat name;  // 文字灰度裁图，只用于接续和稳定判断，不识别名称。
  // 普通卡只保存固定星星前景，问号卡保存占位图案；不等待服装缩略图。
  cv::Mat signal;
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

struct AnchorSampleMatch {
  int row{};              // 相对锚点起始行，从 1 开始，供诊断显示。
  int column{};           // 从 1 开始的列号。
  int vertical_offset{};  // 单张文字的局部校正量（px），不改变整体位移。
  double raw_score{};
  double score{};
};

struct AnchorMatch {
  bool valid{};
  int y{};
  int displacement{};  // 原锚点 y 减去当前 y（px）；向下滚动列表时为正。
  double score{};
  double weakest_score{};
  double runner_up{};
  double raw_score{};
  double raw_weakest_score{};
  std::vector<AnchorSampleMatch> samples;
};

// 以下为诊断状态，不进入业务结果 JSON。
struct Session {
  std::string status{"incomplete"};
  core::Error message;
  std::string category;
  std::string created_at;
  int accepted_count{};
  int scroll_count{};
  int page_notches{};
  double pixels_per_notch{};
  double elapsed_seconds{};
  bool single_page{};
};

struct BatchCategory {
  std::string id;
  std::string group;
  std::string status{"pending"};
  core::Error message;
  int accepted_count{};
};

struct BatchSession {
  std::string status{"starting"};
  core::Error message;
  std::string created_at;
  std::string current_category;
  Stage stage{Stage::preparing};
  core::Message navigation_stage;
  int planned_categories{};
  int completed_categories{};
  int accepted_count{};
  double elapsed_seconds{};
  std::vector<BatchCategory> categories;
};

}  // namespace features::scan

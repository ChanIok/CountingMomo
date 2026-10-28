#pragma once

#include "vendor/std.hpp"

#include "vendor/opencv.hpp"

namespace features::scan {
using Box = std::array<int, 4>;
struct CardFrame {
  Box bottom_rect;
  Box right_rect;
  double min_contrast;
};
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
struct NameSettings {
  std::array<int, 2> font_size_range;
  int baseline;
  int origin_count;
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
  Box scroll_rect;
  BottomSettings bottom_detection;
  Box name_rect;
  Box name_tracking_rect;
  Box star_rect;
  OwnershipSettings ownership_star;
  NameSettings name_recognition;
};
struct Candidate {
  std::string text;
  double similarity{};
};
struct Character {
  int index{};
  int left{};
  std::string text;
  double similarity{};
};
struct NameResult {
  std::string text;
  double similarity{};
  double margin{};
  int font_size{};
  int origin{};
  int baseline{};
  std::vector<Candidate> candidates;
  std::vector<Character> characters;  // 仅开启单字校验时输出最佳候选的逐字证据。
  std::vector<std::string> issues;
};
struct Item {
  std::string id;
  int row{};
  int column{};
  std::string category;
  std::string frame_id;
  Box box{};
  Box name_box{};
  Box star_box{};
  NameResult name;
  std::string ownership{"unknown"};
  double star_luma{};
  bool needs_review{true};
  std::vector<std::string> review_reasons;
  std::string card_crop;
  std::string name_crop;
  std::string star_crop;
};
struct Sample {
  Item item;
  cv::Mat name;
  cv::Mat star;
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
  int phase{};
  BottomResult bottom;
  bool single_page{};
  std::vector<cv::Mat> names;
  std::vector<cv::Mat> stars;
};
struct Anchor {
  int y{};
  std::vector<std::vector<cv::Mat>> templates;
};
struct AnchorSampleMatch {
  int row{};
  int column{};
  int vertical_offset{};
  double raw_score{};
  double score{};
};
struct AnchorMatch {
  bool valid{};
  int y{};
  int displacement{};
  double score{};
  double weakest_score{};
  double runner_up{};
  double raw_score{};
  double raw_weakest_score{};
  std::vector<AnchorSampleMatch> samples;
};
struct Session {
  std::string status{"incomplete"};
  std::string message;
  std::string category;
  std::string created_at;
  int expected_count{};
  int accepted_count{};
  int processed_count{};
  int scroll_count{};
  int page_notches{};
  double pixels_per_notch{};
  double elapsed_seconds{};
  double processing_seconds{};
  std::string font;
  std::string names_file;
  std::vector<std::string> ignored_names;
  std::string fallback_font;
  std::vector<std::string> fallback_characters;
  int candidate_names{};
  int candidate_characters{};
  std::string recognition_method{"name_library_gray_templates"};
  bool validate_characters{};
  bool single_page{};
  std::string source_image;
};
auto rect(const Box& box) -> cv::Rect;
auto load_layout(const std::filesystem::path& path) -> std::expected<Layout, std::string>;
}  // namespace features::scan

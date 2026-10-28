#pragma once

#include "vendor/std.hpp"

#include "vendor/opencv.hpp"

#include "features/recognition/font_templates.hpp"
#include "features/recognition/name_library.hpp"
#include "features/scan/types.hpp"

namespace features::recognition {
struct Timings {
  double total_seconds{};
  double preprocess_seconds{};
  double geometry_seconds{};
  double query_preparation_seconds{};
  double template_match_seconds{};
  double layout_selection_seconds{};
  double candidate_seconds{};
  std::uint64_t names{};
  std::uint64_t query_cells{};
  std::uint64_t template_comparisons{};
};
struct NameRecognizer {
  std::unique_ptr<FontTemplates> font;
  NameLibrary library;
  struct NameLayout {
    std::vector<int> lefts;
    int width{};
  };
  struct NameLayouts {
    std::vector<NameLayout> entries;
    std::vector<int> by_width;
  };
  struct GlyphQuery {
    cv::Mat vector;
    PrefixNorms inverse_norms;
  };
  struct Workspace {
    cv::Mat cell;
    cv::Mat ink;
    std::vector<GlyphQuery> queries;
    std::vector<std::vector<int>> requests;
    // 每个参与评分的位置/字形均会在读取前写入，不清空整张分数表。
    std::vector<float> scores;
  };
  std::vector<NameLayouts> layouts;
  Workspace workspace;
  int width{};
  int origin_count{};
  bool validate_characters{};
  Timings timings;
};
auto load(const std::filesystem::path& font, const std::filesystem::path& names,
          const scan::Layout& layout, bool validate_characters)
    -> std::expected<NameRecognizer, std::string>;
auto recognize(NameRecognizer& recognizer, const cv::Mat& name)
    -> std::expected<scan::NameResult, std::string>;
}  // namespace features::recognition

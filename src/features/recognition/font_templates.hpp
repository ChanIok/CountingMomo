#pragma once

#include "vendor/std.hpp"

#include "vendor/freetype.hpp"
#include "vendor/opencv.hpp"

namespace features::recognition {
using PrefixNorms = std::vector<double>;  // 各可见字宽对应的向量长度倒数，含0宽。
struct FontRaster {
  int height{};
  int baseline{};
  int min_size{};
  int max_size{};
};
struct TemplateBank {
  cv::Mat vectors;
  std::vector<double> advances;
  std::vector<int> widths;
  std::vector<PrefixNorms> inverse_norms;
  std::vector<float> column_energy;
  std::vector<float> row_energy;
};
struct FontTemplates {
  FT_Library library{};
  FT_Face face{};
  FT_Face fallback_face{};
  std::string font_data;
  std::string fallback_data;
  std::filesystem::path fallback_path;
  std::vector<char32_t> characters;
  std::vector<FT_Face> glyph_faces;
  std::vector<char32_t> fallback_characters;
  FontRaster raster;
  std::vector<std::optional<TemplateBank>> banks;
  double generation_seconds{};
  int generated_banks{};
  ~FontTemplates();
  FontTemplates() = default;
  FontTemplates(const FontTemplates&) = delete;
  auto operator=(const FontTemplates&) -> FontTemplates& = delete;
};
auto load_font(const std::filesystem::path& path, std::span<const char32_t> characters,
               FontRaster raster) -> std::expected<std::unique_ptr<FontTemplates>, std::string>;
auto template_bank(FontTemplates& font, int size)
    -> std::expected<const TemplateBank*, std::string>;
auto ink_pixels(const cv::Mat& gray) -> cv::Mat;
auto ink_pixels(const cv::Mat& gray, cv::Mat& ink) -> void;
// 查询字格不先整体归一化；余弦评分直接使用各可见字宽的范数。
auto gray_values(const cv::Mat& ink, cv::Mat& values) -> void;
auto gray_vector(const cv::Mat& ink) -> cv::Mat;
auto prefix_inverse_norms(const cv::Mat& vector, int height) -> PrefixNorms;
auto prefix_inverse_norms(const cv::Mat& vector, int height, PrefixNorms& inverse) -> void;
}  // namespace features::recognition

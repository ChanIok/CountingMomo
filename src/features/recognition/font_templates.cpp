#include "features/recognition/font_templates.hpp"

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

#include "utils/file.hpp"
#include "utils/timing.hpp"

namespace features::recognition {
FontTemplates::~FontTemplates() {
  if (fallback_face) FT_Done_Face(fallback_face);
  if (face) FT_Done_Face(face);
  if (library) FT_Done_FreeType(library);
}
namespace {
auto load_fallback(FontTemplates& font) -> std::expected<void, std::string> {
  std::array<wchar_t, MAX_PATH> directory{};
  auto length = GetWindowsDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
  if (!length || length >= directory.size()) return std::unexpected("无法获取系统字体目录。");
  font.fallback_path = std::filesystem::path(directory.data()) / L"Fonts/msyh.ttc";
  auto data = utils::file::read(font.fallback_path);
  if (!data) return std::unexpected("缺字补字需要系统微软雅黑：" + data.error());
  font.fallback_data = std::move(*data);
  if (font.fallback_data.size() > static_cast<std::size_t>(std::numeric_limits<FT_Long>::max()))
    return std::unexpected("系统微软雅黑文件过大。");
  // msyh.ttc的第一个face为常规微软雅黑；直接读取系统文件，不复制进分发目录。
  if (FT_New_Memory_Face(font.library, reinterpret_cast<const FT_Byte*>(font.fallback_data.data()),
                         static_cast<FT_Long>(font.fallback_data.size()), 0, &font.fallback_face) ||
      FT_Select_Charmap(font.fallback_face, FT_ENCODING_UNICODE))
    return std::unexpected("FreeType无法读取系统微软雅黑。");
  return {};
}
}  // namespace
auto load_font(const std::filesystem::path& path, std::span<const char32_t> characters,
               FontRaster raster) -> std::expected<std::unique_ptr<FontTemplates>, std::string> {
  auto data = utils::file::read(path);
  if (!data) return std::unexpected(data.error() + "；请使用 --font 指定游戏字体。");
  auto font = std::make_unique<FontTemplates>();
  font->raster = raster;
  font->banks.resize(raster.max_size - raster.min_size + 1);
  font->font_data = std::move(*data);
  if (font->font_data.size() > static_cast<std::size_t>(std::numeric_limits<FT_Long>::max()))
    return std::unexpected("字体文件过大。");
  if (FT_Init_FreeType(&font->library) ||
      FT_New_Memory_Face(font->library, reinterpret_cast<const FT_Byte*>(font->font_data.data()),
                         static_cast<FT_Long>(font->font_data.size()), 0, &font->face))
    return std::unexpected("FreeType无法读取字体。");
  if (font->face->num_faces != 1 || FT_Select_Charmap(font->face, FT_ENCODING_UNICODE))
    return std::unexpected("需要单个Unicode字体。");
  for (char32_t cp : characters) {
    FT_Face source = font->face;
    if (!FT_Get_Char_Index(source, cp)) {
      if (!font->fallback_face) {
        if (auto ok = load_fallback(*font); !ok) return std::unexpected(ok.error());
      }
      source = font->fallback_face;
      if (!FT_Get_Char_Index(source, cp))
        return std::unexpected(
            std::format("游戏字体和微软雅黑均缺少 U+{:04X}。", static_cast<unsigned>(cp)));
      font->fallback_characters.push_back(cp);
    }
    font->characters.push_back(cp);
    font->glyph_faces.push_back(source);
  }
  return font;
}
auto ink_pixels(const cv::Mat& gray) -> cv::Mat {
  cv::Mat ink;
  ink_pixels(gray, ink);
  return ink;
}

auto ink_pixels(const cv::Mat& gray, cv::Mat& ink) -> void {
  static const cv::Mat kernel = cv::Mat::ones(5, 5, CV_8U);
  // 字格是父图像ROI；隔离边界才能与Python独立裁图的模板一致。
  cv::morphologyEx(gray, ink, cv::MORPH_BLACKHAT, kernel, {-1, -1}, 1,
                   cv::BORDER_CONSTANT | cv::BORDER_ISOLATED);
}

auto gray_values(const cv::Mat& ink, cv::Mat& values) -> void {
  // 保持一行缓冲区的形状，使下一张字格可直接复用同一块浮点内存。
  auto flat = ink.reshape(1, 1);
  flat.convertTo(values, CV_32F);
}
auto gray_vector(const cv::Mat& ink) -> cv::Mat {
  cv::Mat values;
  gray_values(ink, values);
  // 小标点的笔画可能不足画布的1%；分位数截顶会削弱笔画并放大截图背景噪声。
  values /= std::max(cv::norm(values), 1e-8);
  return values;
}
auto prefix_inverse_norms(const cv::Mat& vector, int height) -> PrefixNorms {
  PrefixNorms inverse;
  prefix_inverse_norms(vector, height, inverse);
  return inverse;
}

auto prefix_inverse_norms(const cv::Mat& vector, int height, PrefixNorms& inverse) -> void {
  auto pixels = vector.reshape(1, height);
  inverse.assign(pixels.cols + 1, 0);
  for (int y = 0; y < pixels.rows; ++y) {
    const float* row = pixels.ptr<float>(y);
    for (int x = 0; x < pixels.cols; ++x) inverse[x + 1] += row[x] * row[x];
  }
  double power = 0;
  for (int width = 1; width <= pixels.cols; ++width) {
    power += inverse[width];
    inverse[width] = power > 0 ? 1 / std::sqrt(power) : 0;
  }
}
auto template_bank(FontTemplates& font, int size)
    -> std::expected<const TemplateBank*, std::string> {
  const auto& raster = font.raster;
  if (size < raster.min_size || size > raster.max_size)
    return std::unexpected("字号超出布局标定范围。");
  auto& cached = font.banks[size - raster.min_size];
  if (cached) return &*cached;
  utils::timing::Scope timer(font.generation_seconds);
  if (FT_Set_Pixel_Sizes(font.face, 0, size)) return std::unexpected("设置字体字号失败。");
  TemplateBank bank;
  bank.vectors.create(static_cast<int>(font.characters.size()), (size + 1) * raster.height, CV_32F);
  // 普通字、小字及补字共用截图中标定的基线，水平保留半像素偏移。
  FT_Vector offset{32, 0};
  FT_Set_Transform(font.face, nullptr, &offset);
  if (font.fallback_face) {
    if (FT_Set_Pixel_Sizes(font.fallback_face, 0, size))
      return std::unexpected("设置微软雅黑字号失败。");
    FT_Set_Transform(font.fallback_face, nullptr, &offset);
  }
  for (int index = 0; index < static_cast<int>(font.characters.size()); ++index) {
    auto source = font.glyph_faces[index];
    if (FT_Load_Char(source, font.characters[index], FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL))
      return std::unexpected("字体字形渲染失败。");
    // 补字与原字体共享行基线，避免因两个字体的ascender不同而上下错位。
    auto slot = source->glyph;
    double advance = slot->advance.x / 64.0;
    if (advance <= 0 || advance > size + 1)
      return std::unexpected("名称库字符的字宽超出当前模板范围。");
    bank.advances.push_back(advance);
    bank.widths.push_back(static_cast<int>(std::ceil(advance)));
    cv::Mat canvas(raster.height, size + 1, CV_8U, cv::Scalar(255));
    const auto& bitmap = slot->bitmap;
    if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY && bitmap.width && bitmap.rows)
      return std::unexpected("字体需要灰度字形。");
    for (unsigned y = 0; y < bitmap.rows; ++y) {
      int cy = raster.baseline - slot->bitmap_top + static_cast<int>(y);
      if (cy < 0 || cy >= canvas.rows) continue;
      auto source_y = bitmap.pitch >= 0 ? y : bitmap.rows - y - 1;
      const auto* source = bitmap.buffer + source_y * std::abs(bitmap.pitch);
      for (unsigned x = 0; x < bitmap.width; ++x) {
        int cx = slot->bitmap_left + static_cast<int>(x);
        if (cx >= 0 && cx < canvas.cols)
          canvas.at<std::uint8_t>(cy, cx) = static_cast<std::uint8_t>(255 - source[x]);
      }
    }
    gray_vector(ink_pixels(canvas)).copyTo(bank.vectors.row(index));
    bank.inverse_norms.push_back(prefix_inverse_norms(bank.vectors.row(index), raster.height));
  }
  // 平均笔画投影只定位字格起点与基线，不进行字符身份检索。
  cv::Mat average, columns;
  cv::reduce(bank.vectors, average, 0, cv::REDUCE_AVG, CV_32F);
  cv::reduce(average.reshape(1, raster.height), columns, 0, cv::REDUCE_SUM, CV_32F);
  bank.column_energy.assign(columns.ptr<float>(), columns.ptr<float>() + size);
  bank.column_energy[0] += columns.at<float>(0, size);
  cv::Mat rows;
  cv::reduce(average.reshape(1, raster.height), rows, 1, cv::REDUCE_SUM, CV_32F);
  bank.row_energy.assign(rows.ptr<float>(), rows.ptr<float>() + raster.height);
  cached = std::move(bank);
  ++font.generated_banks;
  return &*cached;
}
}  // namespace features::recognition

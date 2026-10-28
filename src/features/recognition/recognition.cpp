#include "features/recognition/recognition.hpp"

#include "vendor/std.hpp"

#include "vendor/sse2.hpp"

#include "utils/string.hpp"
#include "utils/timing.hpp"

namespace features::recognition {
namespace {
constexpr double kMinInk = 7.5, kMinSimilarity = 0.9, kMinMargin = 0.01;
struct TextGeometry {
  std::vector<int> font_sizes;
  bool unsupported_height{};
  int right{-1};
  std::vector<double> columns;
  std::vector<double> rows;
};
auto measure_text(const cv::Mat& ink, const FontRaster& raster) -> TextGeometry {
  TextGeometry geometry;
  geometry.font_sizes.push_back(raster.max_size);
  geometry.columns.resize(ink.cols);
  geometry.rows.resize(ink.rows);
  for (int y = 0; y < ink.rows; ++y) {
    for (int x = 0; x < ink.cols; ++x) {
      double value = std::max(0.0, ink.at<std::uint8_t>(y, x) - kMinInk);
      geometry.columns[x] += value;
      geometry.rows[y] += value;
    }
  }
  double column_peak = *std::ranges::max_element(geometry.columns);
  int left = ink.cols, right = -1;
  for (int x = 0; x < ink.cols; ++x) {
    if (geometry.columns[x] > 0 && geometry.columns[x] >= column_peak * 0.12) {
      left = std::min(left, x);
      right = x;
    }
  }

  geometry.right = right;

  // 只有接近占满可读区域的名称才可能缩字；短名称不测高度。
  if (right - left + 1 < ink.cols - raster.max_size) return geometry;

  const auto& rows = geometry.rows;
  double row_peak = *std::ranges::max_element(rows);
  int top = ink.rows, bottom = -1;
  for (int y = 0; y < ink.rows; ++y) {
    if (rows[y] > 0 && rows[y] >= row_peak * 0.12) {
      top = std::min(top, y);
      bottom = y;
    }
  }
  int measured = bottom - top + 2;
  // 笔画高度不等于字号；长名称只比较高度邻域，最终由整名评分决定。
  geometry.font_sizes.clear();
  for (int size = std::max(raster.min_size, measured - 1);
       size <= std::min(raster.max_size, measured + 2); ++size)
    geometry.font_sizes.push_back(size);
  geometry.unsupported_height = geometry.font_sizes.empty();
  return geometry;
}
auto select_origins(const TextGeometry& geometry, const TemplateBank& bank, int size, int count)
    -> std::array<int, 2> {
  std::vector<std::pair<double, int>> choices(count);
  double peak = *std::ranges::max_element(geometry.columns);
  int right = 0;
  for (int x = 0; x < static_cast<int>(geometry.columns.size()); ++x)
    if (geometry.columns[x] > 0 && geometry.columns[x] >= peak * 0.12) right = x;
  for (int origin = 0; origin < count; ++origin) {
    double dot = 0, expected_power = 0;
    // 保留字间空隙的证据，只排除文字结束后的背景。
    for (int x = 0; x <= right; ++x) {
      double expected = x >= origin ? bank.column_energy[(x - origin) % size] : 0;
      dot += geometry.columns[x] * expected;
      expected_power += expected * expected;
    }
    choices[origin] = {dot / std::sqrt(std::max(expected_power, 1e-8)), origin};
  }
  std::ranges::sort(choices, [](auto a, auto b) {
    return a.first == b.first ? a.second < b.second : a.first > b.first;
  });
  return {choices[0].second, choices[1].second};
}
auto select_vertical_offset(const TextGeometry& geometry, const TemplateBank& bank) -> int {
  double best_score = -1;
  int best = 0;
  // 滚动停靠时，文字与边框的独立像素取整可能差1px；整行投影定位后只匹配一次。
  for (int offset : {0, -1, 1}) {
    double dot = 0, power = 0;
    for (int y = 0; y < static_cast<int>(geometry.rows.size()); ++y) {
      int source = y - offset;
      double expected = source >= 0 && source < static_cast<int>(bank.row_energy.size())
                            ? bank.row_energy[source]
                            : 0;
      dot += geometry.rows[y] * expected;
      power += expected * expected;
    }
    double score = dot / std::sqrt(std::max(power, 1e-8));
    if (score > best_score) {
      best_score = score;
      best = offset;
    }
  }
  return best;
}
struct NameMatch {
  int name{};
  int origin{};
  int font_size{};
  int vertical_offset{};
  double similarity{};
};
auto better_match(const NameMatch& a, const NameMatch& b) -> bool {
  if (a.similarity != b.similarity) return a.similarity > b.similarity;
  if (a.name != b.name) return a.name < b.name;
  if (a.font_size != b.font_size) return a.font_size > b.font_size;
  return a.origin < b.origin;
}
auto best_names(std::span<const NameMatch> matches) -> std::vector<NameMatch> {
  // 一个名称的不同起点或字号不是不同名称，不能拿它们计算候选差距。
  std::vector<NameMatch> best;
  best.reserve(6);
  for (const auto& match : matches) {
    if (match.similarity < 0) continue;
    auto same =
        std::ranges::find_if(best, [&](const auto& other) { return other.name == match.name; });
    if (same != best.end()) {
      if (!better_match(match, *same)) continue;
      best.erase(same);
    }
    auto position = std::lower_bound(best.begin(), best.end(), match, better_match);
    best.insert(position, match);
    if (best.size() > 5) best.pop_back();
  }
  return best;
}
auto name_layouts(NameRecognizer& recognizer, const TemplateBank& bank, int size)
    -> const NameRecognizer::NameLayouts& {
  auto& cached = recognizer.layouts[size - recognizer.font->raster.min_size];
  auto& layouts = cached.entries;
  if (!layouts.empty()) return cached;
  layouts.reserve(recognizer.library.names.size());
  for (const auto& name : recognizer.library.names) {
    NameRecognizer::NameLayout layout;
    double advance = 0;
    for (int glyph : name.glyphs) {
      layout.lefts.push_back(static_cast<int>(std::lround(advance)));
      advance += bank.advances[glyph];
    }
    layout.width = static_cast<int>(std::lround(advance));
    layouts.push_back(std::move(layout));
  }
  cached.by_width.resize(layouts.size());
  std::iota(cached.by_width.begin(), cached.by_width.end(), 0);
  std::ranges::sort(cached.by_width, [&](int a, int b) {
    return layouts[a].width == layouts[b].width ? a < b : layouts[a].width < layouts[b].width;
  });
  return cached;
}

auto glyph_dot(const float* query, const float* glyph, int height, int stride, int width)
    -> double {
  // x64基线SSE2：浮点乘积转为双精度累加，不要求AVX2或全局fast-math。
  __m128d low = _mm_setzero_pd(), high = _mm_setzero_pd();
  double tail = 0;
  for (int y = 0; y < height; ++y) {
    int x = 0;
    for (; x + 4 <= width; x += 4) {
      auto product = _mm_mul_ps(_mm_loadu_ps(query + x), _mm_loadu_ps(glyph + x));
      low = _mm_add_pd(low, _mm_cvtps_pd(product));
      high = _mm_add_pd(high, _mm_cvtps_pd(_mm_movehl_ps(product, product)));
    }
    for (; x < width; ++x) tail += query[x] * glyph[x];
    query += stride;
    glyph += stride;
  }
  auto sum = _mm_add_pd(low, high);
  return tail + _mm_cvtsd_f64(sum) + _mm_cvtsd_f64(_mm_unpackhi_pd(sum, sum));
}

auto glyph_similarity(const NameRecognizer::GlyphQuery& query, const TemplateBank& bank, int glyph,
                      int left, const cv::Mat& ink, bool space) -> float {
  int width = std::min(bank.widths[glyph], ink.cols - left);
  if (space) {
    double peak{};
    cv::minMaxLoc(ink(cv::Rect(left, 0, width, ink.rows)), nullptr, &peak);
    return peak < kMinInk ? 1 : 0;
  }
  double inverse = query.inverse_norms[width] * bank.inverse_norms[glyph][width];
  if (!bank.inverse_norms[glyph][width]) return -1;
  const float* q = query.vector.ptr<float>();
  const float* t = bank.vectors.ptr<float>(glyph);
  int stride = query.vector.cols / ink.rows;
  double dot = glyph_dot(q, t, ink.rows, stride, width);
  return static_cast<float>(dot * inverse);
}
auto rank(NameRecognizer& recognizer, const cv::Mat& gray, const cv::Mat& ink,
          const TextGeometry& geometry, int size)
    -> std::expected<std::vector<NameMatch>, std::string> {
  auto bank_result = template_bank(*recognizer.font, size);
  if (!bank_result) return std::unexpected(bank_result.error());
  const auto& bank = **bank_result;
  int vertical_offset = utils::timing::measure(
      recognizer.timings.geometry_seconds, [&] { return select_vertical_offset(geometry, bank); });
  auto origins = utils::timing::measure(recognizer.timings.geometry_seconds, [&] {
    return select_origins(geometry, bank, size, recognizer.origin_count);
  });
  std::vector<NameMatch> matches;
  auto& workspace = recognizer.workspace;
  auto& requests = workspace.requests;
  auto& queries = workspace.queries;
  auto& scores = workspace.scores;
  const auto character_count = recognizer.library.characters.size();
  const NameRecognizer::NameLayouts* cached{};
  {
    utils::timing::Scope timer(recognizer.timings.layout_selection_seconds);
    cached = &name_layouts(recognizer, bank, size);
    const auto& layouts = cached->entries;
    for (auto& request : requests) request.clear();
    bool clipped = geometry.right >= gray.cols - 3;
    for (int origin : origins) {
      // 与原来的end筛选严格等价；触边时不限制名称后缀宽度。
      int min_width = geometry.right - size / 2 - origin;
      int max_width =
          clipped ? std::numeric_limits<int>::max() : geometry.right + size + 1 - origin;
      auto first =
          std::lower_bound(cached->by_width.begin(), cached->by_width.end(), min_width,
                           [&](int index, int width) { return layouts[index].width < width; });
      for (auto it = first; it != cached->by_width.end(); ++it) {
        int i = *it;
        const auto& layout = layouts[i];
        if (layout.width > max_width) break;
        matches.push_back({i, origin, size, vertical_offset});
        const auto& name = recognizer.library.names[i];
        for (std::size_t j = 0; j < name.glyphs.size(); ++j) {
          int left = origin + layout.lefts[j];
          if (left < gray.cols) requests[left].push_back(name.glyphs[j]);
        }
      }
    }
  }
  {
    utils::timing::Scope timer(recognizer.timings.query_preparation_seconds);
    workspace.cell.create(gray.rows, size + 1, CV_8U);
    for (int left = 0; left < gray.cols; ++left) {
      auto& glyphs = requests[left];
      if (glyphs.empty()) continue;
      std::ranges::sort(glyphs);
      auto unique = std::ranges::unique(glyphs);
      glyphs.erase(unique.begin(), unique.end());
      auto& cell = workspace.cell;
      cell.setTo(cv::Scalar(255));
      int width = std::min(size + 1, gray.cols - left);
      int source_y = std::max(0, vertical_offset), dest_y = std::max(0, -vertical_offset);
      int height = gray.rows - std::abs(vertical_offset);
      gray(cv::Rect(left, source_y, width, height))
          .copyTo(cell(cv::Rect(0, dest_y, width, height)));
      ink_pixels(cell, workspace.ink);
      gray_values(workspace.ink, queries[left].vector);
      prefix_inverse_norms(queries[left].vector, gray.rows, queries[left].inverse_norms);
      ++recognizer.timings.query_cells;
    }
  }
  {
    utils::timing::Scope timer(recognizer.timings.template_match_seconds);
    for (int left = 0; left < gray.cols; ++left) {
      for (int glyph : requests[left]) {
        scores[left * character_count + glyph] = glyph_similarity(
            queries[left], bank, glyph, left, ink, recognizer.library.characters[glyph] == U' ');
        ++recognizer.timings.template_comparisons;
      }
    }
  }
  utils::timing::Scope timer(recognizer.timings.layout_selection_seconds);
  for (auto& match : matches) {
    const auto& name = recognizer.library.names[match.name];
    const auto& layout = cached->entries[match.name];
    double sum = 0;
    int count = 0;
    for (std::size_t i = 0; i < name.glyphs.size(); ++i) {
      int left = match.origin + layout.lefts[i];
      if (left >= gray.cols) continue;
      double value = scores[left * character_count + name.glyphs[i]];
      if (value < 0) continue;
      sum += value;
      ++count;
    }
    match.similarity = count ? sum / count : -1;
  }
  return best_names(matches);
}
auto issue(scan::NameResult& name, std::string text) -> void {
  if (std::ranges::find(name.issues, text) == name.issues.end())
    name.issues.push_back(std::move(text));
}
}  // namespace

auto load(const std::filesystem::path& path, const std::filesystem::path& names,
          const scan::Layout& layout, bool validate_characters)
    -> std::expected<NameRecognizer, std::string> {
  auto library = load_names(names);
  if (!library) return std::unexpected(library.error());
  const auto& settings = layout.name_recognition;
  auto n = layout.name_rect;
  auto font = load_font(
      path, library->characters,
      {n[3] - n[1], settings.baseline, settings.font_size_range[0], settings.font_size_range[1]});
  if (!font) return std::unexpected(font.error());
  if (auto bank = template_bank(**font, settings.font_size_range[1]); !bank)
    return std::unexpected(bank.error());
  NameRecognizer recognizer;
  recognizer.font = std::move(*font);
  recognizer.library = std::move(*library);
  recognizer.width = n[2] - n[0];
  recognizer.origin_count = settings.origin_count;
  recognizer.validate_characters = validate_characters;
  recognizer.layouts.resize(settings.font_size_range[1] - settings.font_size_range[0] + 1);
  recognizer.workspace.queries.resize(recognizer.width);
  recognizer.workspace.requests.resize(recognizer.width);
  recognizer.workspace.scores.resize(recognizer.width * recognizer.library.characters.size());
  return recognizer;
}
auto recognize(NameRecognizer& r, const cv::Mat& name)
    -> std::expected<scan::NameResult, std::string> {
  utils::timing::Scope total_timer(r.timings.total_seconds);
  ++r.timings.names;
  scan::NameResult result;
  if (name.empty() || name.cols != r.width || name.rows != r.font->raster.height ||
      name.type() != CV_8UC3)
    return std::unexpected("名称裁图尺寸或类型与布局不符。");
  cv::Mat gray, ink;
  bool readable = false;
  {
    utils::timing::Scope timer(r.timings.preprocess_seconds);
    cv::cvtColor(name, gray, cv::COLOR_BGR2GRAY);
    ink = ink_pixels(gray);
    auto flat = ink.reshape(1, 1);
    std::vector<std::uint8_t> pixels(flat.ptr<std::uint8_t>(),
                                     flat.ptr<std::uint8_t>() + flat.total());
    std::nth_element(pixels.begin(), pixels.end() - 5, pixels.end());
    readable = *(pixels.end() - 5) >= kMinInk;
  }
  if (!readable) {
    result.issues.push_back("未检出可读名称");
    return result;
  }
  auto geometry = utils::timing::measure(r.timings.geometry_seconds,
                                         [&] { return measure_text(ink, r.font->raster); });
  if (geometry.unsupported_height) {
    result.issues.push_back("文字高度超出当前字号验证范围");
    return result;
  }
  std::vector<float> character_scores;
  std::vector<NameMatch> candidates;
  std::optional<NameMatch> selected;
  for (int size : geometry.font_sizes) {
    auto ranked = rank(r, gray, ink, geometry, size);
    if (!ranked) return std::unexpected(ranked.error());
    if (r.validate_characters && !ranked->empty() &&
        (!selected || better_match(ranked->front(), *selected))) {
      utils::timing::Scope timer(r.timings.candidate_seconds);
      selected = ranked->front();
      // 仅单字校验开启时保留逐字分数；下一字号会复用分数表。
      const auto& name = r.library.names[selected->name];
      const auto& layout = r.layouts[size - r.font->raster.min_size].entries[selected->name];
      character_scores.resize(name.glyphs.size());
      for (std::size_t i = 0; i < name.glyphs.size(); ++i) {
        int left = selected->origin + layout.lefts[i];
        character_scores[i] =
            left < r.width ? r.workspace.scores[left * r.library.characters.size() + name.glyphs[i]]
                           : -1;
      }
    }
    candidates.insert(candidates.end(), ranked->begin(), ranked->end());
  }
  auto matches =
      utils::timing::measure(r.timings.candidate_seconds, [&] { return best_names(candidates); });
  if (matches.empty()) {
    result.issues.push_back("没有宽度及可见笔画相符的库内名称，可能不在名称库中");
    return result;
  }
  utils::timing::Scope timer(r.timings.candidate_seconds);
  const auto& best = matches.front();
  const auto& entry = r.library.names[best.name];
  int bank_index = best.font_size - r.font->raster.min_size;
  const auto& layout = r.layouts[bank_index].entries[best.name];
  const auto& bank = *r.font->banks[bank_index];
  result.font_size = best.font_size;
  result.origin = best.origin;
  result.baseline = r.font->raster.baseline + best.vertical_offset;
  result.similarity = best.similarity;
  result.margin = matches.size() > 1 ? best.similarity - matches[1].similarity : 0;
  for (const auto& match : matches)
    result.candidates.push_back({r.library.names[match.name].text, match.similarity});
  bool enough = best.similarity >= kMinSimilarity;
  if (!enough) issue(result, "名称整体匹配分数不足，需核对名称库与原图");
  if (result.margin < kMinMargin) issue(result, "名称库候选差距不足，无法确定名称");
  // 库内一定存在最近邻；只有分数及差距足够时才填入最终名称。
  if (enough && result.margin >= kMinMargin) result.text = entry.text;
  for (std::size_t i = 0; i < entry.glyphs.size(); ++i) {
    int glyph = entry.glyphs[i];
    int left = best.origin + layout.lefts[i];
    if (r.font->glyph_faces[glyph] == r.font->fallback_face)
      issue(result, std::format("第{}字使用微软雅黑补字，需核对字形", i + 1));
    if (left + bank.widths[glyph] > r.width) issue(result, "候选名称超出裁图，后缀无法完整核对");
    // 截断、不可见及补字属于名称完整性检查，不依赖可选的逐字分数证据。
    int width = std::min(bank.widths[glyph], r.width - left);
    if (width <= 0 || (r.library.characters[glyph] != U' ' && !bank.inverse_norms[glyph][width]))
      issue(result, "候选名称存在不可见字符");
    if (!r.validate_characters) continue;
    double similarity = character_scores[i];
    result.characters.push_back({static_cast<int>(i), left,
                                 utils::string::codepoint(r.library.characters[glyph]),
                                 std::max(0.0, similarity)});
    if (similarity >= 0 && similarity < kMinSimilarity)
      issue(result, std::format("第{}字匹配不确定", i + 1));
  }
  return result;
}
}  // namespace features::recognition

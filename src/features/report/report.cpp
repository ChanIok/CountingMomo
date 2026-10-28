#include "features/report/report.hpp"

#include "vendor/std.hpp"

#include "vendor/rfl.hpp"

#include "utils/file.hpp"
#include "utils/image.hpp"
#include "utils/string.hpp"

namespace features::report {
namespace {
auto csv(std::string_view value) -> std::string {
  std::string result = "\"";
  for (char c : value) {
    if (c == '"') result += '"';
    result += c;
  }
  return result + '"';
}
struct Document {
  scan::Session session;
  scan::Layout layout;
  std::vector<scan::Item> items;
};
}  // namespace
auto save_crops(const std::filesystem::path& output, const std::vector<scan::Item>& items)
    -> std::expected<void, std::string> {
  std::string frame_id;
  cv::Mat frame;
  for (const auto& item : items) {
    // 同页条目连续排列；每次只解码并持有一页，不累积整次扫描的页面像素。
    if (item.frame_id != frame_id) {
      auto loaded = utils::image::load(output / "frames" / (item.frame_id + ".jpg"));
      if (!loaded) return std::unexpected(loaded.error());
      frame = std::move(*loaded);
      frame_id = item.frame_id;
    }
    for (auto [path, box] :
         {std::pair{item.card_crop, item.box}, std::pair{item.name_crop, item.name_box},
          std::pair{item.star_crop, item.star_box}}) {
      auto area = scan::rect(box);
      if ((area & cv::Rect(0, 0, frame.cols, frame.rows)) != area)
        return std::unexpected("报告裁图超出页面：" + item.id);
      if (auto ok = utils::image::save_png(output / utils::string::wide(path), frame(area)); !ok)
        return ok;
    }
  }
  return {};
}

auto write(const std::filesystem::path& output, const scan::Session& session,
           const scan::Layout& layout, const std::vector<scan::Item>& items)
    -> std::expected<void, std::string> {
  if (auto ok = utils::file::write(output / "items.json",
                                   rfl::json::write(Document{session, layout, items}));
      !ok)
    return ok;
  std::string table =
      "\xef\xbb\xbf"
      "id,category,name,ownership,similarity,margin,font_size,needs_review\r\n";
  for (const auto& item : items)
    table += std::format("{},{},{},{},{:.6f},{:.6f},{},{}\r\n", csv(item.id), csv(item.category),
                         csv(item.name.text), csv(item.ownership), item.name.similarity,
                         item.name.margin, item.name.font_size, item.needs_review);
  if (auto ok = utils::file::write(output / "items.csv", table); !ok) return ok;
  auto escape = utils::string::html;
  int review = 0, owned = 0, unowned = 0;
  for (const auto& item : items) {
    review += item.needs_review;
    owned += item.ownership == "owned";
    unowned += item.ownership == "unowned";
  }
  std::string page =
      "<!doctype html><html lang=zh-CN><meta charset=utf-8><meta name=viewport "
      "content='width=device-width,initial-scale=1'><title>CountingMomo "
      "核对报告</title><style>body{font:15px "
      "system-ui;margin:24px;background:#f5f5f7;color:#222}article{background:white;border:1px "
      "solid #ddd;padding:14px;margin:12px 0;border-radius:8px}.review{border-left:5px solid "
      "#c66}img{image-rendering:pixelated;vertical-align:middle;margin:6px}small{color:#555}"
      "summary{cursor:pointer}table{border-collapse:collapse}td,th{padding:5px;border:1px solid "
      "#ddd}</style>";
  page += std::format(
      "<h1>{} · 核对报告</h1><p>{}：{}</p><p>采集 {}，处理 {}，已拥有 {}，未拥有 {}，待核对 "
      "{}。</p>",
      escape(session.category), escape(session.status), escape(session.message),
      session.accepted_count, items.size(), owned, unowned, review);
  if (!session.source_image.empty()) page += "<p>源图：" + escape(session.source_image) + "</p>";
  page +=
      "<p>"
      "名称相似度是裁图内字符灰度余弦值的平均，候选差距是两个不同名称的得分之差，均不是概率。"
      "整名分数或差距不足时名称留空；截断、不可见字符及补字仍标记核对。";
  page += session.validate_characters
              ? "单字校验已开启：单字低分仅标记核对，不清空整名；逐字证据对应最佳候选名称。</p>"
              : "单字校验已关闭：不生成逐字证据，也不因单字低分标记核对。</p>";
  for (const auto& item : items) {
    page += std::format(
        "<article class='{}'><b>{} · {}</b> <small>{}</small><div><img src='{}' width={} "
        "height={} alt='卡片'><img src='{}' width={} height={} alt='名称'><img src='{}' width={} "
        "height={} alt='星星'></div>",
        item.needs_review ? "review" : "", escape(item.id),
        item.name.text.empty() ? "未确定名称" : escape(item.name.text), escape(item.ownership),
        escape(item.card_crop), item.box[2] - item.box[0], item.box[3] - item.box[1],
        escape(item.name_crop), (item.name_box[2] - item.name_box[0]) * 2,
        (item.name_box[3] - item.name_box[1]) * 2, escape(item.star_crop),
        (item.star_box[2] - item.star_box[0]) * 3, (item.star_box[3] - item.star_box[1]) * 3);
    page += std::format(
        "<small>字号 {}，基线 {}，相似度 {:.4f}，候选差距 {:.4f}，星星亮度 {:.2f}</small>",
        item.name.font_size, item.name.baseline, item.name.similarity, item.name.margin,
        item.star_luma);
    for (const auto& reason : item.review_reasons) page += "<p>" + escape(reason) + "</p>";
    page += session.validate_characters ? "<details><summary>名称候选与逐字证据</summary>"
                                        : "<details><summary>名称候选</summary>";
    page += "<table><tr><th>名称候选</th><th>相似度</th></tr>";
    for (const auto& candidate : item.name.candidates)
      page += std::format("<tr><td>{}</td><td>{:.4f}</td></tr>", escape(candidate.text),
                          candidate.similarity);
    page += "</table>";
    if (session.validate_characters && !item.name.characters.empty()) {
      page +=
          "<p>最佳候选的逐字证据（不可见字符显示0分，详见核对原因）：</p>"
          "<table><tr><th>位置</th><th>字符</th><th>相似度</th></tr>";
      for (const auto& character : item.name.characters) {
        page += std::format("<tr><td>{}</td><td>{}</td><td>{:.4f}</td></tr>", character.index + 1,
                            escape(character.text), character.similarity);
      }
      page += "</table>";
    }
    page += "</details></article>";
  }
  page += "</html>";
  return utils::file::write(output / "review.html", page);
}
}  // namespace features::report

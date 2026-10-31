#pragma once

#include "vendor/std.hpp"

#include "vendor/opencv.hpp"

#include "core/message/message.hpp"

#include "features/scan/types.hpp"

namespace features::scan {

// 素材路径相对布局文件，返回原生尺寸的灰度问号模板。
auto load_hidden_question(const Layout& layout, const std::filesystem::path& layout_path)
    -> std::expected<cv::Mat, core::Error>;

// 只检查滚动条底部采样带；调用者据此决定是否开放尾部读取区域。
auto detect_bottom(const cv::Mat& image, const Layout& layout) -> BottomResult;

// 无滚动条沿用单屏假设，不据此接受空分类。
auto items_at_top(const cv::Mat& image, const Layout& layout) -> bool;

// 按原生布局读取安全区内的完整卡片，未就绪时返回原因，不产生第三种拥有状态。
// 实时采集可等待新帧后再调用；离线分析直接报告该错误，不重试或补齐位置。
auto read_page(const cv::Mat& image, const Layout& layout, const cv::Mat& hidden_question,
               int phase, bool at_top, BottomResult bottom) -> std::expected<Page, core::Error>;

// 从重叠区前最多 3 行提取普通卡文字；问号卡保留位置但不参与匹配。
// 没有有效文字时返回错误，不按预计位移推算接续。
auto make_anchor(const Page& page, const Layout& layout, int overlap_rows)
    -> std::expected<Anchor, core::Error>;

// 联合响应峰确定整体位移，单张文字的纵向 ±1px 容差仅用于质量检查。
auto locate_anchor(const cv::Mat& image, const Anchor& anchor, const Layout& layout, bool at_bottom)
    -> AnchorMatch;

// 比较文字和星星／问号信号，不等待服装缩略图；调用者保证两页来自不同源帧。
auto inputs_stable(const Page& previous, const Page& current) -> bool;

}  // namespace features::scan

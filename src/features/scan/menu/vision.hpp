#pragma once

#include "vendor/std.hpp"

#include "vendor/opencv.hpp"

#include "features/scan/menu/types.hpp"

namespace core {
struct AppState;
}

namespace features::scan::menu {

// 将 BGR 图像转换为单通道灰度图
auto gray(const cv::Mat& image) -> cv::Mat;

// 将图像转换为 32 位浮点灰度图，供高亮对比度采样复用
auto gray_float(const cv::Mat& pixels) -> cv::Mat;

// 提取图标高频边缘特征信号（原图减去高斯模糊低频图）
auto icon_signal(const cv::Mat& image) -> cv::Mat;

// 在指定区域内通过模板匹配定位图标中心：特征差分 → 相关性匹配 → 峰值筛选
auto locate(const cv::Mat& image, const Icon& icon, const Box& bounds, bool child = false)
    -> std::optional<Match>;

// 采样图标四边缘的高光边框对比度
auto contrast(const cv::Mat& image, cv::Point point, bool child) -> double;

// 在纵向微调窗口内采样最强的高光边框对比度响应
auto highlight_signal(const cv::Mat& flat, cv::Point point, bool child) -> double;

// 按 ID 在目录中查找运行时图标对象
auto find_icon(const Catalog& catalog, std::string_view id) -> const Icon*;

// 按 ID 在目录中查找运行时分组对象
auto find_group(const Catalog& catalog, std::string_view id) -> const Group*;

// 查找指定分类 ID 对应的外层行号
auto root_row_of(std::string_view id) -> int;

// 获取指定外层行相对 hair 行的累计像素偏移
auto root_offset(int row, bool expanded) -> int;

// 对多个候选原点投票：排序取中位数 → 统计容差内票数 → 票数充足返回原点
auto vote_origin(std::vector<int> origins, int min_anchors) -> std::optional<int>;

// 检验父入口下方是否可见已展开的子项列表：匹配子项图标 → 投票原点
auto child_list_visible(const Group& group, const cv::Mat& image, int parent_y) -> bool;

// 推算外层整列排布及当前选中行：定位可见图标 → 投票原点 → 量取高亮行
auto infer_root_layout(const core::AppState& state, const cv::Mat& image, const cv::Mat& flat)
    -> std::optional<RootLayout>;

// 计算指定分组子项列表在当前排布下的可见搜索视口
auto bounds(const Group& group, const RootLayout& layout) -> std::optional<Box>;

// 获取分类在所属分组中的下标索引
auto category_index(const Group& group, const Icon& icon) -> int;

// 匹配所有可见子项推算子视口列表原点 y 坐标
auto child_origin(const Group& group, const cv::Mat& image, const Box& area) -> std::optional<int>;

// 推算目标分类或分组图标在当前画面中的中心坐标
auto target_point(const Group& group, const Icon& icon, const cv::Mat& image,
                  const RootLayout& layout, bool root) -> std::optional<cv::Point>;

// 判定目标分类或分组在当前画面中是否处于选中高亮状态
auto selected(const core::AppState& state, const Group& group, const Icon& icon,
              const cv::Mat& image, bool root = false) -> bool;

// 根据目标移动距离与像素比率计算所需的滚轮格数
auto notches_for(int distance, double rate, int limit) -> int;

}  // namespace features::scan::menu

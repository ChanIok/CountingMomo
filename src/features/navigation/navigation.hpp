#pragma once

#include "vendor/std.hpp"

#include "vendor/opencv.hpp"

#include "core/message/message.hpp"
#include "features/navigation/types.hpp"

namespace core {
struct AppState;
}

namespace features::navigation {

// 按模块内固定分类表加载图标，directory 为分类 PNG 素材目录。
auto load(const std::filesystem::path& directory) -> std::expected<Catalog, core::Error>;

// 在现有事务超时内取得首帧稳定画面，按当前状态做最少的修复：排布认不出或首个服装
// 分类太靠上才过量回顶；只有起步就已选中它时才点一次下一行，保证随后的 visit 是真实切换。
// 干净状态不注入任何滚轮或点击，子类别展开状态由第一次点击自行收起。
auto begin(core::AppState& state) -> std::expected<void, core::Error>;

// 展开分组；子列表收起期间保留的滚动位置，由 seek 按可见子项推算后自行收敛。
auto open_group(core::AppState& state, const Group& group) -> std::expected<void, core::Error>;

// 先消除鼠标悬停影响，目标已选中则不重复点击；两条路径均确认目标状态。
// 返回时 last_frame 为目标分类的完整稳定帧。
auto visit(core::AppState& state, const Group& group, const Icon& icon)
    -> std::expected<void, core::Error>;

}  // namespace features::navigation

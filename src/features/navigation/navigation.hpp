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

// 在现有事务超时内取得 GPU 稳定画面并确认初始图标可定位。
// 只确认侧栏已回顶、分组已收起，不代替用户重置导航初始状态。
auto begin(core::AppState& state) -> std::expected<void, core::Error>;

auto open_group(core::AppState& state, const Group& group) -> std::expected<void, core::Error>;

// 先消除鼠标悬停影响，目标已选中则不重复点击；两条路径均确认目标状态。
// 返回时 last_frame 为目标分类的完整稳定帧。
auto visit(core::AppState& state, const Group& group, const Icon& icon)
    -> std::expected<void, core::Error>;

}  // namespace features::navigation

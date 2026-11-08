#pragma once

// 菜单领域接口：会话类型与导航动作动词
#include "vendor/std.hpp"

#include "core/message/types.hpp"
#include "features/scan/menu/types.hpp"

namespace core {
struct AppState;
}

namespace features::scan::menu {

// 加载分类图标资源并初始化菜单会话；超时由扫描编排层注入，失败即中断本次扫描
auto prepare(core::AppState& state, double frame_timeout) -> void;

// 初始化菜单状态：停放鼠标 → 等待首帧 → 必要时回顶并打破发型初始选中
auto begin(core::AppState& state) -> std::expected<void, core::Error>;

// 展开目标分组：定位入口图标 → 点击展开 → 滚动露出子视口并对齐
auto open_group(core::AppState& state, const Group& group) -> std::expected<void, core::Error>;

// 切换至指定分类：滚动定位图标 → 停放鼠标防伪高光 → 点击选中并确认生效
auto visit(core::AppState& state, const Group& group, const Icon& icon)
    -> std::expected<void, core::Error>;

}  // namespace features::scan::menu

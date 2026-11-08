#pragma once

#include "vendor/std.hpp"

#include "core/message/types.hpp"
#include "features/scan/menu/types.hpp"

namespace core {
struct AppState;
}

namespace features::scan::runtime {

// 仅扫描线程可见的中断；边界 execute() 一次转成 Outcome。
struct Abort {
  core::Error error;
};

struct Stop {
  core::Error error;
};

[[noreturn]] auto fail(core::AppState& state, core::Error error) -> void;

// 拆开 expected：成功就交出内部值，失败即中断本次扫描；用户已请求停止时优先算停止而非失败。
// 这是动词边界上唯一把 std::expected 转成内部中断的地方，调用处因此不必写错误传播。
inline auto must(core::AppState& state, std::expected<void, core::Error> result) -> void {
  if (!result) {
    fail(state, std::move(result.error()));
  }
}

template <typename T>
auto must(core::AppState& state, std::expected<T, core::Error> result) -> T {
  if (!result) {
    fail(state, std::move(result.error()));
  }
  return std::move(*result);
}

// 诊断保存完成后重置会话，释放图像资源。
auto release_run(core::AppState& state) -> void;

// 准备窗口与资源：加载分类配置与模板 → 游戏窗口切竖屏 → 激活前台
auto prepare_window(core::AppState& state) -> void;

// 启动画面捕获：配置对比区域与稳定判定 → 开启 WGC 捕获
auto start_capture(core::AppState& state) -> void;

// 获取待扫描的分组及分类目录列表
auto groups(core::AppState& state) -> const std::vector<menu::Group>&;

// 进入图鉴界面并重置初始选中项
auto enter_menu(core::AppState& state) -> void;

// 展开指定分组以显示分类子项
auto expand(core::AppState& state, const menu::Group& group) -> void;

// 选中目标分类：通知切换状态 → 滚动定位并点击图标
auto select(core::AppState& state, const menu::Group& group, const menu::Icon& icon) -> void;

// 记录单分类扫描结果：统计已拥有数 → 暂存结果数组 → 推进总进度
auto accept(core::AppState& state, std::string_view id, std::vector<int> values) -> void;

// 标记全流程扫描完成并通知 UI
auto publish(core::AppState& state) -> void;

}  // namespace features::scan::runtime

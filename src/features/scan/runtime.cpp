#include "features/scan/runtime.hpp"

#include "vendor/std.hpp"

#include "core/message/message.hpp"
#include "core/state/app_state.hpp"
#include "features/scan/capture/capture.hpp"
#include "features/scan/control.hpp"
#include "features/scan/layout.hpp"
#include "features/scan/menu/menu.hpp"
#include "features/scan/pages/pages.hpp"
#include "features/scan/progress.hpp"
#include "features/scan/state.hpp"
#include "features/window/window.hpp"

namespace features::scan::runtime {

namespace {

constexpr double frame_timeout = 8.0;
constexpr double stable_seconds = 0.25;

// 初始化分类扫描状态：记录分类 ID → 清零计数 → 播报切换状态
auto start_category(core::AppState& state, std::string_view id) -> void {
  auto& progress = state.scan->session.progress;
  // 记录当前目标分类并重置单分类已采集数
  progress.category = std::string(id);
  progress.category_count = 0;
  // 状态切为切换分类中，并同步给悬浮窗
  progress.stage = Stage::switching_category;
  publish_progress(state);
}

}  // namespace

// 抛出扫描中断：用户主动停止抛 Stop，发生错误抛 Abort
[[noreturn]] auto fail(core::AppState& state, core::Error error) -> void {
  // 用户主动点了停止，抛 Stop 走正常中止流程
  if (stop_requested(state)) {
    throw Stop{std::move(error)};
  }
  // 发生未预期的失败，抛 Abort 触发错误上报与诊断
  throw Abort{std::move(error)};
}

// 释放本次扫描会话的全部资源与缓存
auto release_run(core::AppState& state) -> void { state.scan->session = {}; }

// 准备窗口与资源：初始化菜单与页面资源 → 游戏窗口切竖屏 → 激活前台
auto prepare_window(core::AppState& state) -> void {
  auto& session = state.scan->session;
  // 加载分类图标资源并初始化菜单会话
  menu::prepare(state, frame_timeout);
  session.resources.frame_timeout = frame_timeout;
  // 统计全部计划扫描的分类总数，供悬浮窗计算全局进度
  for (const auto& group : session.menu.catalog.groups) {
    session.progress.planned_categories += static_cast<int>(group.categories.size());
  }
  publish_progress(state);

  // 加载页面扫描所需的问号模板并初始化页面域状态
  pages::begin(state);

  // 查找游戏主窗口
  auto found = must(state, window::find(state.scan->request.hwnd, window::game_executable));
  const auto& layout = get_layout();
  // 状态切为调整窗口大小
  session.progress.stage = Stage::adjusting_window;
  publish_progress(state);
  // 将游戏窗口临时调整为 960x3840 无边框竖屏
  auto target = must(state, window::prepare(state, found, layout.reference_size[0],
                                            layout.reference_size[1], frame_timeout));
  session.resources.target = target;
  // 状态切为等待前台激活
  session.progress.stage = Stage::waiting_foreground;
  publish_progress(state);
  // 将游戏窗口激活到前台接收输入
  must(state, window::activate(state, session.resources.target));
}

// 启动画面捕获：配置对比区域与稳定判定 → 开启 WGC 捕获
auto start_capture(core::AppState& state) -> void {
  auto& session = state.scan->session;
  const auto& layout = get_layout();
  // 按基准稳定区域启动 WGC 帧捕获与 GPU 稳定性判定
  must(state, capture::start(state, session.resources.target.hwnd,
                             {.width = session.resources.target.width,
                              .height = session.resources.target.height,
                              .comparison_rect = layout.stability_rect,
                              .stable_seconds = stable_seconds}));
  // 状态切为等待画面排布稳定
  session.progress.stage = Stage::waiting_layout;
  publish_progress(state);
}

// 获取待扫描的分组及分类目录列表
auto groups(core::AppState& state) -> const std::vector<menu::Group>& {
  return state.scan->session.menu.catalog.groups;
}

// 进入图鉴界面并重置初始选中项
auto enter_menu(core::AppState& state) -> void {
  // 点击入口进入图鉴，并打破初始默认选中态
  must(state, menu::begin(state));
}

// 展开指定分组以显示分类子项
auto expand(core::AppState& state, const menu::Group& group) -> void {
  // 点击分组折叠栏展开列表并对齐视图
  must(state, menu::open_group(state, group));
}

// 选中目标分类：通知切换状态 → 滚动定位并点击图标
auto select(core::AppState& state, const menu::Group& group, const menu::Icon& icon) -> void {
  // 播报切换到当前目标分类
  start_category(state, icon.definition->id);
  // 定位目标图标并点击选中，确认高亮生效
  must(state, menu::visit(state, group, icon));
}

// 记录单分类扫描结果：统计已拥有数 → 暂存结果数组 → 推进总进度
auto accept(core::AppState& state, std::string_view id, std::vector<int> values) -> void {
  auto& session = state.scan->session;
  // 记录本分类摘要：总数量与拥有数量
  session.summaries.push_back({std::string(id), static_cast<int>(values.size()),
                               static_cast<int>(std::ranges::count(values, 1))});
  // 保存该分类完整的 0/1 拥有状态数组
  session.results.emplace(std::string(id), std::move(values));
  // 推进已完成分类数并清空单分类计数
  ++session.progress.completed_categories;
  session.progress.category_count = 0;
  // 向 UI 广播更新后的进度
  publish_progress(state);
}

// 标记全流程扫描完成并通知 UI
auto publish(core::AppState& state) -> void {
  auto& progress = state.scan->session.progress;
  // 清理当前分类名称
  progress.category.clear();
  // 状态切为扫描完成
  progress.stage = Stage::completed;
  // 向 UI 广播最终完成状态
  publish_progress(state);
}

}  // namespace features::scan::runtime

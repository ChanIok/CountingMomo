#pragma once

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

#include "core/message/types.hpp"
#include "features/capture/types.hpp"

namespace core {
struct AppState;
}

namespace features::capture {

// 与 WGC SystemRelativeTime 同一 QPC 时钟，单位为 100ns，用于记录输入完成边界。
auto source_time() -> std::int64_t;
// 启动、等帧和关闭均由扫描任务线程调用；初始化失败会显式回滚已分配的资源。
// 创建捕获资源前确认 WGC 与排除鼠标指针的能力，缺少任一能力时直接失败。
auto start(core::AppState& state, HWND hwnd, Settings settings) -> std::expected<void, core::Error>;
// GPU 精确比较配置区域，达到稳定时长后才读回完整客户区图片。
// after_source_time 排除操作前的帧；require_change 用于拒绝候选后的重试，必须先看到画面变化。
// check 在当前扫描线程同步执行，用于取消和窗口检查；无帧时至多间隔 50ms 检查一次。
// 整个等待共用 deadline，旧帧和无效通知都不延长它。
// 空值表示到 deadline 仍未等到稳定帧，失败原因与建议由调用方确定。
auto wait_stable_frame(core::AppState& state, std::int64_t after_source_time,
                       std::chrono::steady_clock::time_point deadline,
                       const std::function<std::expected<void, core::Error>()>& check,
                       bool require_change = false)
    -> std::expected<std::optional<Frame>, core::Error>;
// 可从控制线程调用；只唤醒等待，不操作 WGC 或 D3D 资源。
auto wake(core::AppState& state) -> void;
// 停止会话、撤销帧通知并关闭帧池；部分初始化状态同样显式回收。
auto stop(core::AppState& state) -> std::expected<void, core::Error>;

}  // namespace features::capture

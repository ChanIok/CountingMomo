#pragma once

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

#include "core/message/types.hpp"
#include "features/scan/capture/types.hpp"

namespace core {
struct AppState;
}

namespace features::scan::capture {

// 获取当前系统高精度 QPC 时间戳（单位为 100ns）
auto source_time() -> std::int64_t;

// 启动 WGC 捕获会话：初始化 D3D11 设备 → 创建捕获帧池与会话 → 开启不含光标的帧捕获
auto start(core::AppState& state, HWND hwnd, Settings settings) -> std::expected<void, core::Error>;

// 阻塞等待画面稳定：轮询接收新帧 → GPU 判定比对区变化 → 达到稳定时长后读回 BGR 图像
auto wait_stable_frame(core::AppState& state, std::int64_t after_source_time,
                       std::chrono::steady_clock::time_point deadline,
                       const std::function<std::expected<void, core::Error>()>& check,
                       bool require_change = false)
    -> std::expected<std::optional<Frame>, core::Error>;

// 跨线程唤醒帧等待流程（通常由取消/停止操作触发）
auto wake(core::AppState& state) -> void;

// 停止捕获会话并释放全部 WGC 与 D3D 资源
auto stop(core::AppState& state) -> std::expected<void, core::Error>;

}  // namespace features::scan::capture

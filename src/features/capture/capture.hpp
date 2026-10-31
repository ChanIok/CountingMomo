#pragma once

#include "vendor/std.hpp"

#include "vendor/opencv.hpp"
#include "vendor/windows.hpp"

#include "core/message/message.hpp"

namespace features::capture {

struct Frame {
  std::uint64_t sequence{};
  std::int64_t source_time{};  // WGC 的 SystemRelativeTime 原始值，单位为 100ns。
  std::chrono::steady_clock::time_point received_at;  // 本地接收时间，供耗时和输入边界判断。
  cv::Mat bgr;
};
struct CaptureState;

struct Capture {
  std::shared_ptr<CaptureState> state;
};

// 等待窗口调整后的首个目标尺寸帧，此后严格检查捕获尺寸。
// 等待期间 snapshot 可为空，由调用者沿用自身事务超时和停止控制。
auto start(HWND hwnd, int width, int height) -> std::expected<Capture, core::Error>;

// 只返回当前最新帧；启动后尚未收到首帧时，成功结果中的指针可以为空。
// 读取不会消费或等待新帧，调用者须同时比较 sequence 和 source_time 去重。
auto snapshot(const Capture& capture) -> std::expected<std::shared_ptr<const Frame>, core::Error>;

// 在调用线程关闭会话并解除回调；未启动或已停止时不做任何操作。
auto stop(Capture& capture) -> void;
}  // namespace features::capture

#pragma once

#include "vendor/std.hpp"

#include "vendor/opencv.hpp"
#include "vendor/windows.hpp"

namespace features::capture {
struct Frame {
  std::uint64_t sequence{};
  std::int64_t source_time{};
  std::chrono::steady_clock::time_point received_at;
  cv::Mat bgr;
};
struct CaptureState;
struct Capture {
  std::shared_ptr<CaptureState> state;
};
auto start(HWND hwnd, int width, int height) -> std::expected<Capture, std::string>;
auto snapshot(const Capture& capture) -> std::expected<std::shared_ptr<const Frame>, std::string>;
auto stop(Capture& capture) -> void;
}  // namespace features::capture

#pragma once

#include "vendor/std.hpp"

#include "core/message/message.hpp"

namespace utils::timer {

// 精确等到绝对时间点：高分辨率可等待计时器绕过系统 15.625ms 时钟中断，
// 单次提交即睡到目标时刻，不在内部做短切片轮询。
auto sleep_until(std::chrono::steady_clock::time_point deadline)
    -> std::expected<void, core::Error>;

}  // namespace utils::timer

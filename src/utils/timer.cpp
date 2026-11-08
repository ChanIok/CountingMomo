#include "utils/timer.hpp"

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

#include "core/message/message.hpp"

namespace utils::timer {

namespace {

using Clock = std::chrono::steady_clock;

// 每线程复用一个计时器句柄：扫描线程是唯一调用者，线程退出时随句柄一起释放。
struct Reusable {
  HANDLE handle{};

  ~Reusable() {
    if (handle) {
      CloseHandle(handle);
    }
  }
};

thread_local Reusable reusable;

// 首次使用时创建高分辨率计时器。创建失败意味着系统不满足运行前提，
// 不能退回普通 Sleep 的时钟量子——那会静默恢复成需要诊断的偶发行为。
auto acquire() -> std::expected<HANDLE, core::Error> {
  if (!reusable.handle) {
    HANDLE created = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                            TIMER_ALL_ACCESS);
    if (!created) {
      auto code = GetLastError();
      return std::unexpected(core::with_detail(
          core::error("error.timer.failed"), std::format("CreateWaitableTimerExW Win32={}", code)));
    }
    reusable.handle = created;
  }
  return reusable.handle;
}

}  // namespace

auto sleep_until(Clock::time_point deadline) -> std::expected<void, core::Error> {
  auto remaining = deadline - Clock::now();
  // 已到期或不足一个计时单位都视为立即完成，不提交无意义的等待。
  if (remaining <= Clock::duration::zero()) {
    return {};
  }
  auto hundred_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100;
  if (hundred_ns <= 0) {
    return {};
  }
  auto handle = acquire();
  if (!handle) {
    return std::unexpected(handle.error());
  }
  // SetWaitableTimer 的相对时长以 100ns 为单位，负值表示从现在起算。
  LARGE_INTEGER due{};
  due.QuadPart = -hundred_ns;
  if (!SetWaitableTimer(*handle, &due, 0, nullptr, nullptr, FALSE)) {
    auto code = GetLastError();
    return std::unexpected(core::with_detail(core::error("error.timer.failed"),
                                             std::format("SetWaitableTimer Win32={}", code)));
  }
  auto waited = WaitForSingleObject(*handle, INFINITE);
  if (waited != WAIT_OBJECT_0) {
    auto code = GetLastError();
    return std::unexpected(
        core::with_detail(core::error("error.timer.failed"),
                          std::format("WaitForSingleObject result={} Win32={}", waited, code)));
  }
  return {};
}

}  // namespace utils::timer

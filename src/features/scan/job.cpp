#include "features/scan/job.hpp"

#include "vendor/std.hpp"

#include "vendor/windows.hpp"
#include "vendor/windows/roapi.hpp"

#include "core/message/message.hpp"
#include "core/state/app_state.hpp"
#include "features/scan/control.hpp"
#include "features/scan/diagnostics.hpp"
#include "features/scan/state.hpp"
#include "features/scan/usecase.hpp"

namespace features::scan::job {
// 启动后台扫描工作线程：重置停止标志 → 清空共享队列 → 启动线程同步执行全量扫描
auto start(core::AppState& state, features::scan::Request request)
    -> std::expected<void, core::Error> {
  auto& scan = *state.scan;
  // 确保先前的扫描线程已经完全回收
  if (scan.worker.joinable()) {
    return std::unexpected(core::error("error.scan.worker_not_joined"));
  }
  // 清除原子停止标志
  scan.stop.store(false);
  {
    // 重置进度与待通知状态
    std::lock_guard lock(scan.mutex);
    scan.latest = {};
    scan.pending.reset();
    scan.notification_pending = false;
  }
  scan.request = std::move(request);
  try {
    // 启动独立后台工作线程
    scan.worker = std::thread([&state] {
      features::scan::Outcome outcome;
      auto started = std::chrono::steady_clock::now();
      state.scan->working_result = {};
      state.scan->working_progress = {};
      // 初始化当前工作线程的多线程 COM / WinRT 套间
      auto apartment = RoInitialize(RO_INIT_MULTITHREADED);
      if (FAILED(apartment)) {
        outcome.message = core::error("error.scan.com_initialize",
                                      std::format("{:08X}", static_cast<unsigned long>(apartment)));
      } else {
        try {
          // 同步执行全量图鉴扫描流水线
          auto result = features::scan::run(state);
          if (result) {
            outcome = std::move(*result);
          } else {
            // 收尾警告独立于扫描失败或主动停止。
            outcome = std::move(state.scan->working_result);
            outcome.status = state.scan->stop.load() ? Phase::stopped : Phase::failed;
            outcome.message = result.error();
          }
        } catch (const std::exception& error) {
          outcome.message = core::with_detail(core::error("error.scan.exception"), error.what());
        }
      }
      outcome.elapsed_seconds =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
      // run 已关闭捕获并恢复窗口；WIC 保存现场图仍需要当前线程的 COM 套间。
      auto saved = diagnostics::save(state, outcome);
      if (!saved) {
        auto warning = core::error("error.diagnostics.save_failed");
        warning.related.push_back(saved.error());
        if (outcome.warning.message.key.empty()) {
          outcome.warning = std::move(warning);
        } else {
          outcome.warning.related.push_back(std::move(warning));
        }
      }
      release_run(state);
      if (SUCCEEDED(apartment)) {
        RoUninitialize();
      }
      bool notify = false;
      {
        // 加锁将最终结果存入 pending 槽位
        auto& scan = *state.scan;
        std::lock_guard lock(scan.mutex);
        scan.pending = std::move(outcome);
        if (!scan.notification_pending) {
          scan.notification_pending = true;
          notify = true;
        }
      }
      // 唤醒 UI 线程（通过 PostMessage 投递 scan_update 消息）
      if (notify && state.scan->wake) {
        state.scan->wake();
      }
    });
  } catch (const std::exception& error) {
    return std::unexpected(core::with_detail(core::error("error.scan.start_worker"), error.what()));
  }
  return {};
}

// 在 UI 线程消费最新的扫描进展与终态：加锁取出 pending 结果 → 存在终态时回收并 join 线程
auto consume(core::AppState& state) -> std::optional<features::scan::Outcome> {
  auto& scan = *state.scan;
  std::optional<features::scan::Outcome> result;
  {
    // 加锁读取最新进度与终态结果
    std::lock_guard lock(scan.mutex);
    state.scan->progress = scan.latest;
    result = std::move(scan.pending);
    scan.pending.reset();
    scan.notification_pending = false;
  }
  // 存在终态时同步回收工作线程，杜绝悬挂线程
  if (result && scan.worker.joinable()) {
    scan.worker.join();
  }
  return result;
}

auto shutdown(core::AppState& state) -> void {
  request_stop(state);
  if (state.scan->worker.joinable()) {
    state.scan->worker.join();
  }
}
}  // namespace features::scan::job

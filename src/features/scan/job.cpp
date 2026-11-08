#include "features/scan/job.hpp"

#include "vendor/std.hpp"

#include "vendor/windows.hpp"
#include "vendor/windows/roapi.hpp"
#include "vendor/windows/winrt/windows_foundation.hpp"

#include "core/message/message.hpp"
#include "core/state/app_state.hpp"
#include "features/scan/capture/capture.hpp"
#include "features/scan/control.hpp"
#include "features/scan/diagnostics.hpp"
#include "features/scan/pages/pages.hpp"
#include "features/scan/runtime.hpp"
#include "features/scan/state.hpp"
#include "features/window/state.hpp"
#include "features/window/window.hpp"

namespace features::scan::job {

// 跑完一次扫描：处理遗留窗口 → 顺序执行采集 → 关捕获还原窗口 → 把中断装配成 Outcome
auto execute(core::AppState& state) -> Outcome {
  Outcome outcome;
  std::optional<core::Error> failure;
  // 上次异常退出可能留下没还原的游戏窗口，先处理它，别覆盖恢复所需的信息
  if (state.window->original) {
    if (auto restored = window::restore(state); !restored) {
      auto error = core::error("error.window.restore_failed");
      error.related.push_back(restored.error());
      failure = std::move(error);
    }
  }
  bool completed = false;
  // 只有前置恢复没出错才开跑，否则把那个错误原样记成失败
  if (!failure) {
    try {
      // 准备窗口与捕获、打开图鉴；资源就绪后才开始遍历分类
      runtime::prepare_window(state);
      runtime::start_capture(state);
      runtime::enter_menu(state);
      // 逐分组展开、逐分类切换并采集；结果在 accept 里累加，脚本不碰状态
      for (const menu::Group& group : runtime::groups(state)) {
        runtime::expand(state, group);
        for (const menu::Icon& icon : group.categories) {
          runtime::select(state, group, icon);
          runtime::accept(state, icon.definition->id, pages::read_all_pages(state));
        }
      }
      // 全部走完才置成功标志，下面据此决定要不要搬运结果
      runtime::publish(state);
      completed = true;
    } catch (const runtime::Stop& stop) {
      // 用户主动停止单独记成 stopped，UI 不弹错误
      outcome.status = Phase::stopped;
      outcome.message = stop.error;
    } catch (const runtime::Abort& abort) {
      // 动词失败带着原始错误落地，诊断会用它生成报告
      outcome.status = Phase::failed;
      outcome.message = abort.error;
    } catch (const winrt::hresult_error& error) {
      // 兜住 WinRT 与标准异常，避免工作线程带着未捕获异常退出
      outcome.status = Phase::failed;
      outcome.message =
          core::with_detail(core::error("error.scan.exception"), winrt::to_string(error.message()));
    } catch (const std::exception& error) {
      outcome.status = Phase::failed;
      outcome.message = core::with_detail(core::error("error.scan.exception"), error.what());
    } catch (...) {
      outcome.status = Phase::failed;
      outcome.message = core::error("error.scan.exception");
    }
  } else {
    outcome.status = Phase::failed;
    outcome.message = std::move(*failure);
  }

  // 收尾失败只累加成警告，不覆盖采集本身的失败原因
  auto add_cleanup_error = [&](core::Error error) {
    auto& target = outcome.warning;
    // 已有警告时挂到 related，保证多条收尾错误都不丢
    if (target.message.key.empty()) {
      target = std::move(error);
    } else {
      target.related.push_back(std::move(error));
    }
  };
  // 无论成功失败都要停捕获并还原窗口，否则游戏会一直停在竖屏
  if (auto stopped = capture::stop(state); !stopped) {
    add_cleanup_error(stopped.error());
  }
  if (auto restored = window::restore(state); !restored) {
    auto warning = core::error("error.window.restore_failed");
    warning.related.push_back(restored.error());
    // 还原失败要接管已有警告，把它串进 related 而不是丢掉
    if (!outcome.warning.message.key.empty()) {
      warning.related.push_back(outcome.warning);
    }
    outcome.warning = std::move(warning);
  }
  // 唯一的结果交接点：只有全程无中断才把累加器搬进 Outcome，半成品一律丢弃
  if (completed) {
    outcome.status = Phase::completed;
    outcome.categories = std::move(state.scan->session.results);
    outcome.summaries = std::move(state.scan->session.summaries);
  }
  return outcome;
}

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
      auto started = std::chrono::steady_clock::now();
      state.scan->session = {};
      // 初始化当前工作线程的多线程 COM / WinRT 套间
      auto apartment = RoInitialize(RO_INIT_MULTITHREADED);
      features::scan::Outcome outcome;
      if (FAILED(apartment)) {
        outcome.status = Phase::failed;
        outcome.message = core::error("error.scan.com_initialize",
                                      std::format("{:08X}", static_cast<unsigned long>(apartment)));
      } else {
        try {
          outcome = features::scan::job::execute(state);
        } catch (const std::exception& error) {
          outcome.message = core::with_detail(core::error("error.scan.exception"), error.what());
        }
      }
      outcome.elapsed_seconds =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
      // execute 已关闭捕获并恢复窗口；WIC 保存现场图仍需要当前线程的 COM 套间。
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
      runtime::release_run(state);
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

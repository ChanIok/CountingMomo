#include "app/scan_job.hpp"

#include "vendor/std.hpp"

#include "vendor/windows.hpp"
#include "vendor/windows/roapi.hpp"

#include "core/message/message.hpp"

#include "app/state.hpp"
#include "core/state/app_state.hpp"
#include "features/scan/usecase.hpp"

namespace app::scan_job {
auto start(core::AppState& state, features::scan::Request request)
    -> std::expected<void, core::Error> {
  auto& scan = *state.scan;
  if (scan.worker.joinable()) {
    return std::unexpected(core::error("error.scan.worker_not_joined"));
  }
  scan.stop.store(false);
  {
    std::lock_guard lock(scan.mutex);
    scan.latest = {};
    scan.pending.reset();
    scan.notification_pending = false;
  }
  try {
    scan.worker = std::thread([&state, request = std::move(request)] {
      features::scan::Outcome outcome;
      auto started = std::chrono::steady_clock::now();
      auto apartment = RoInitialize(RO_INIT_MULTITHREADED);
      if (FAILED(apartment)) {
        outcome.message = core::error("error.scan.com_initialize",
                                      std::format("{:08X}", static_cast<unsigned long>(apartment)));
      } else {
        try {
          auto result = features::scan::run(state, request);
          if (result) {
            outcome = std::move(*result);
          } else {
            outcome = std::move(state.scan->working_result);
            outcome.status = state.scan->stop.load() ? "stopped" : "failed";
            outcome.message = result.error();
          }
        } catch (const std::exception& error) {
          outcome.message = core::with_detail(core::error("error.scan.exception"), error.what());
        }
        RoUninitialize();
      }
      outcome.diagnostic = request.output / "diagnostics";
      outcome.elapsed_seconds =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
      bool notify = false;
      {
        auto& scan = *state.scan;
        std::lock_guard lock(scan.mutex);
        scan.pending = std::move(outcome);
        if (!scan.notification_pending) {
          scan.notification_pending = true;
          notify = true;
        }
      }
      if (notify && state.scan->wake) {
        state.scan->wake();
      }
    });
  } catch (const std::exception& error) {
    return std::unexpected(core::with_detail(core::error("error.scan.start_worker"), error.what()));
  }
  return {};
}

auto consume(core::AppState& state) -> std::optional<features::scan::Outcome> {
  auto& scan = *state.scan;
  std::optional<features::scan::Outcome> result;
  {
    std::lock_guard lock(scan.mutex);
    state.app->progress = scan.latest;
    result = std::move(scan.pending);
    scan.pending.reset();
    scan.notification_pending = false;
  }
  if (result && scan.worker.joinable()) {
    scan.worker.join();
  }
  return result;
}
}  // namespace app::scan_job

#include "features/scan/progress.hpp"

#include "vendor/std.hpp"

#include "core/state/app_state.hpp"
#include "features/scan/state.hpp"

namespace features::scan {
auto publish_progress(core::AppState& state) -> void {
  auto& scan = *state.scan;
  bool notify = false;
  {
    std::lock_guard lock(scan.mutex);
    scan.latest = scan.working_progress;
    if (scan.wake && !scan.notification_pending) {
      scan.notification_pending = true;
      notify = true;
    }
  }
  if (notify) {
    scan.wake();
  }
}

}  // namespace features::scan

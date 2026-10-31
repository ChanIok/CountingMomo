#include "features/scan/progress.hpp"

#include "vendor/std.hpp"

#include "core/state/app_state.hpp"
#include "features/scan/state.hpp"

namespace features::scan {
// 发布最新扫描进度：将工作区进度拷贝到共享区并按需唤醒 UI 线程
auto publish_progress(core::AppState& state) -> void {
  auto& scan = *state.scan;
  bool notify = false;
  {
    // 加锁保护跨线程共享的进度状态
    std::lock_guard lock(scan.mutex);
    // 同步工作线程内部进度到对外公布结构中
    scan.latest = scan.working_progress;
    // 如果存在唤醒回调且当前无在途通知，则标记待通知
    if (scan.wake && !scan.notification_pending) {
      scan.notification_pending = true;
      notify = true;
    }
  }
  // 在锁外调用 UI 唤醒回调（向主窗口发送 Win32 消息），避免锁竞争
  if (notify) {
    scan.wake();
  }
}

}  // namespace features::scan

#include "core/shutdown/shutdown.hpp"

#include "vendor/std.hpp"

#include "core/startup/startup.hpp"
#include "features/scan/job.hpp"
#include "ui/main_window/window.hpp"
#include "ui/status_window/window.hpp"
#include "ui/theme.hpp"

namespace core::shutdown {

auto shutdown_application(AppState& state) -> void {
  // 此后不再分发消息，先关闭 UI 入口，再等待任务完成窗口恢复等收尾。
  ui::main_window::prepare_shutdown(state);
  features::scan::job::shutdown(state);
  // 在销毁消息目标之前撤销监听，阻止在途回调继续投递。
  ui::theme::shutdown(state);
  ui::status_window::destroy(state);
  ui::main_window::destroy(state);
  startup::release_instance(state);
}

}  // namespace core::shutdown

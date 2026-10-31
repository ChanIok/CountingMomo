#include "features/scan/stage.hpp"

#include "vendor/std.hpp"

#include "core/message/message.hpp"

namespace features::scan {
auto stage_message(Stage stage) -> core::Message {
  switch (stage) {
    case Stage::preparing:
      return core::message("scan.stage.preparing");
    case Stage::adjusting_window:
      return core::message("scan.stage.adjusting_window");
    case Stage::waiting_foreground:
      return core::message("scan.stage.waiting_foreground");
    case Stage::waiting_layout:
      return core::message("scan.stage.waiting_layout");
    case Stage::switching_category:
      return core::message("scan.stage.switching_category");
    case Stage::resetting_items:
      return core::message("scan.stage.resetting_items");
    case Stage::collecting:
      return core::message("scan.stage.collecting");
    case Stage::completed:
      return core::message("scan.stage.completed");
  }
  std::unreachable();
}
}  // namespace features::scan

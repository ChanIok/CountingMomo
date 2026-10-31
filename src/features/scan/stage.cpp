#include "features/scan/stage.hpp"

#include "vendor/std.hpp"

#include "core/message/message.hpp"

namespace features::scan {
// 映射扫描阶段枚举到多语言展示消息
auto stage_message(Stage stage) -> core::Message {
  // 根据当前执行阶段返回对应的展示文案键
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
    case Stage::collecting:
      return core::message("scan.stage.collecting");
    case Stage::completed:
      return core::message("scan.stage.completed");
  }
  std::unreachable();
}
}  // namespace features::scan

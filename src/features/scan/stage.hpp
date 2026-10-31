#pragma once

#include "core/message/types.hpp"

namespace features::scan {
enum class Stage {
  preparing,
  adjusting_window,
  waiting_foreground,
  waiting_layout,
  switching_category,
  collecting,
  completed
};
auto stage_message(Stage stage) -> core::Message;
}  // namespace features::scan

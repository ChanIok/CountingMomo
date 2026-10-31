#pragma once

#include "vendor/std.hpp"

#include "features/scan/task_types.hpp"

namespace features::result {

struct State {
  // 只由 UI 线程发布；新扫描失败或停止时保留上次完整成功结果。
  std::optional<scan::Outcome> last_success;
};
}  // namespace features::result

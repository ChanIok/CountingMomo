#pragma once

#include "vendor/std.hpp"

#include "core/message/message.hpp"
#include "features/capture/types.hpp"
#include "features/navigation/types.hpp"

namespace features::navigation {

// 仅扫描线程读写；每次扫描重新初始化，结束后释放图像资源。
struct State {
  Catalog catalog{};
  double timeout{};  // 由扫描用例传入，单位为秒。
  std::optional<capture::Frame> last_frame;
  core::Message stage{core::message("navigation.stage.initializing")};
  bool pointer_parked{};
};
}  // namespace features::navigation

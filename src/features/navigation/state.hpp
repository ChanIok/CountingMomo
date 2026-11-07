#pragma once

#include "vendor/std.hpp"

#include "core/message/message.hpp"
#include "features/capture/types.hpp"
#include "features/navigation/types.hpp"

namespace features::navigation {

// 仅扫描线程读写；每次扫描重新初始化，结束后释放图像资源。
struct State {
  Catalog catalog{};
  double timeout{};           // 由扫描用例传入，单位为秒。
  double pixels_per_notch{};  // 首次需要按像素率滚动时实测得出，0 表示尚未标定。
  std::optional<capture::Frame> last_frame;
  core::Message stage{core::message("navigation.stage.initializing")};
  bool pointer_parked{};
};
}  // namespace features::navigation

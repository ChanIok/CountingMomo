#pragma once
#include "vendor/std.hpp"

#include "features/scan/types.hpp"

namespace features::scan {

struct RunTimings {
  std::string units{"seconds"};
  double wall_seconds{};
  double resource_load_seconds{};
  double window_setup_seconds{};
  double focus_wait_seconds{};
  double capture_start_seconds{};
  double page_wait_seconds{};
  double append_seconds{};
  double export_seconds{};
  double diagnostic_save_seconds{};
};

struct TraceEntry {
  std::uint64_t sequence{};
  std::int64_t source_time{};
  double received_seconds{};
  bool at_bottom{};
  bool single_page{};
  int displacement{};
  int stable_frames{};
  std::optional<AnchorMatch> anchor;
  core::Message stage;
  std::optional<core::Error> error;
};

struct Transaction {
  int index{};
  int wheel_notches{};
  std::uint64_t before_sequence{};
  std::int64_t before_source_time{};
  double input_started_seconds{};
  double input_sent_seconds{};
  int actual_pixels{};
  int advance{};  // 相对上一页推进的网格行数，不是像素数。
  std::vector<TraceEntry> trace;
};

struct BatchTimings {
  std::string units{"seconds"};
  double resource_load_seconds{};
  double wall_seconds{};
  double window_setup_seconds{};
  double focus_wait_seconds{};
  double capture_start_seconds{};
  double navigation_seconds{};
  double list_reset_seconds{};
  double categories_wall_seconds{};
  double overview_export_seconds{};
};

}  // namespace features::scan

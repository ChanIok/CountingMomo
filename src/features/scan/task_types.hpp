#pragma once

#include "vendor/std.hpp"

#include "core/message/types.hpp"
#include "features/result/types.hpp"

namespace features::scan {

enum class Phase { ready, running, stopping, completed, stopped, failed };

// 扫描线程当前所处步骤；只描述执行位置，展示措辞由 UI 决定。
enum class Stage {
  preparing,
  adjusting_window,
  waiting_foreground,
  waiting_layout,
  switching_category,
  collecting,
  completed
};

struct Progress {
  Stage stage{Stage::preparing};
  std::string category;
  int completed_categories{};
  int planned_categories{};
  int category_count{};
  int accepted_count{};
};

struct CategorySummary {
  std::string id;
  int count{};
  int owned{};
};

struct Outcome {
  Phase status{Phase::failed};
  core::Error message;
  core::Error warning;  // 各终态独立交付收尾事项，不使完整结果失效。
  result::Categories categories;
  std::vector<CategorySummary> summaries;
  std::filesystem::path diagnostic;
  double elapsed_seconds{};
};
}  // namespace features::scan

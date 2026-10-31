#pragma once

#include "vendor/std.hpp"

#include "core/message/types.hpp"
#include "features/scan/stage.hpp"

#include "features/result/result.hpp"
#include "features/scan/request.hpp"

namespace features::scan {

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
  std::string status{"failed"};
  core::Error message;
  core::Error warning;  // 窗口恢复等非业务失败不使完整结果失效。
  result::Categories categories;
  std::vector<CategorySummary> summaries;
  std::filesystem::path diagnostic;
  double elapsed_seconds{};
};

struct State {
  std::thread worker;
  std::atomic_bool stop{};
  // latest、pending 与 notification_pending 只在 mutex 下交接。
  std::mutex mutex;
  Progress latest;
  std::optional<Outcome> pending;
  bool notification_pending{};
  // 启动前设置，任务回收前不修改；wake 只发送 UI 消息，不操作窗口状态。
  std::function<void()> wake;
  // 以下仅扫描线程使用，完成后移交 Outcome；UI 不读取采集中数组。
  Progress working_progress;
  Outcome working_result;
};

}  // namespace features::scan

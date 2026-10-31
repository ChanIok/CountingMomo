#pragma once

#include "vendor/std.hpp"

#include "core/message/types.hpp"
#include "features/capture/types.hpp"
#include "features/scan/request.hpp"
#include "features/scan/task_types.hpp"
#include "features/scan/types.hpp"
#include "features/window/types.hpp"

namespace features::scan {

struct ScrollPlan {
  int page_notches{};
  double pixels_per_notch{};
};

struct Resources {
  window::Target target;
  double frame_timeout{};  // 批次用例向页面采集传入的事务超时，单位为秒。
  cv::Mat hidden_question;
  ScrollPlan scroll;
  // 本分类切换后的完整稳定帧，直接作为首屏候选；未就绪才等待变化后重采样。
  std::optional<capture::Frame> category_frame;
};

struct CategoryState {
  std::vector<int> values;
  cv::Mat last_image;
  std::int64_t last_frame_source_time{};
  Page previous;
  int accepted_rows{};
  int previous_start{};
};

struct State {
  // UI 线程拥有任务生命周期及已消费的进度快照。
  Phase phase{Phase::ready};
  Progress progress;
  std::filesystem::path diagnostic;
  core::Error message;
  std::chrono::steady_clock::time_point started{};
  std::thread worker;
  std::atomic_bool stop{};
  // latest、pending 与 notification_pending 只在 mutex 下交接。
  std::mutex mutex;
  Progress latest;
  std::optional<Outcome> pending;
  bool notification_pending{};
  // 启动前设置，任务回收前不修改；wake 只发送 UI 消息。
  std::function<void()> wake;
  Request request;  // UI 启动前写入，直到任务回收都不再修改。
  // 以下仅扫描线程读写；回收前 UI 不读取采集中数组或资源。
  Progress working_progress;
  Outcome working_result;
  Resources resources;
  CategoryState category;
  result::Categories completed;
};
}  // namespace features::scan

#pragma once

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

#include "core/message/message.hpp"
#include "core/message/types.hpp"
#include "features/result/types.hpp"
#include "features/scan/capture/state.hpp"
#include "features/scan/menu/types.hpp"
#include "features/scan/pages/types.hpp"
#include "features/scan/task_types.hpp"
#include "features/scan/types.hpp"
#include "features/window/types.hpp"

namespace features::scan {

// 一次性扫描入参：目标窗口、诊断根目录与停止键配置。
struct Request {
  HWND hwnd{};
  std::filesystem::path diagnostic_root;
  int stop_key{10};
  bool hotkey_enabled{};
};

// 跨批次共享的窗口与超时配置。
struct Resources {
  window::Target target;
  double frame_timeout{};  // 批次用例向页面采集传入的事务超时，单位为秒。
};

// 一次扫描的会话数据，挂在 state.scan 上；UI 线程不读取。
struct Session {
  // 菜单会话：当前帧、排布阶段、滚轮标定与指针停放状态。
  menu::MenuState menu;

  // 页面扫描会话：问号模板、滚轮标定与当前分类的翻页过程。
  pages::PagesState pages;

  // 跨批次共享的窗口与超时配置。
  Resources resources;

  // 唯一累加器。
  Progress progress;
  result::Categories results;
  std::vector<CategorySummary> summaries;
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
  // UI 线程维护可用性；工作线程只读取启动前固定的 request。
  bool hotkey_available{true};
  bool hotkey_registered{};
  // latest、pending 与 notification_pending 只在 mutex 下交接。
  std::mutex mutex;
  Progress latest;
  std::optional<Outcome> pending;
  bool notification_pending{};
  // 启动前设置，任务回收前不修改；wake 只发送 UI 消息。
  std::function<void()> wake;
  Request request;  // UI 启动前写入，直到任务回收都不再修改。
  // WGC 捕获会话：每次任务由工作线程 start/stop，UI 线程仅经 wake 触碰其信号设施。
  capture::State capture;
  // 仅扫描线程读写；每次任务重置，UI 回收线程前不读取采集中数组或资源。
  Session session;
};
}  // namespace features::scan

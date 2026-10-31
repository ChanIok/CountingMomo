#include "features/scan/usecase.hpp"

#include "vendor/std.hpp"

#include "vendor/windows.hpp"
#include "vendor/windows/shellapi.hpp"
#include "vendor/windows/winrt/windows_foundation.hpp"

#include "core/message/message.hpp"
#include "core/state/app_state.hpp"
#include "features/capture/capture.hpp"
#include "features/navigation/navigation.hpp"
#include "features/navigation/state.hpp"
#include "features/result/state.hpp"
#include "features/scan/control.hpp"
#include "features/scan/job.hpp"
#include "features/scan/layout.hpp"
#include "features/scan/progress.hpp"
#include "features/scan/scan.hpp"
#include "features/scan/state.hpp"
#include "features/scan/vision.hpp"
#include "features/window/state.hpp"
#include "features/window/window.hpp"
#include "ui/main_window/state.hpp"
#include "ui/main_window/window.hpp"
#include "ui/status_window/state.hpp"
#include "ui/status_window/window.hpp"
#include "utils/file.hpp"

namespace features::scan {

namespace {
constexpr double frame_timeout = 8.0;
constexpr double stable_seconds = 0.25;

// 初始化捕获与导航；阶段仅用于流程进度，不触发诊断写入。
auto prepare_batch_capture(core::AppState& state) -> std::expected<void, core::Error> {
  const auto& options = state.scan->request;
  const auto& layout = get_layout();
  auto& shared = state.scan->resources;
  auto& progress = state.scan->working_progress;
  publish_progress(state);

  auto root = utils::file::executable_directory();
  if (!root) {
    return std::unexpected(root.error());
  }
  auto question = load_hidden_question(*root / "resources/scan/hidden_question.png");
  if (!question) {
    return std::unexpected(question.error());
  }
  shared.hidden_question = std::move(*question);

  auto found = window::find(options.hwnd, window::game_executable);
  if (!found) {
    return std::unexpected(found.error());
  }
  progress.stage = Stage::adjusting_window;
  publish_progress(state);
  auto target = window::prepare(state, *found, layout.reference_size[0], layout.reference_size[1],
                                frame_timeout);
  if (!target) {
    return std::unexpected(target.error());
  }
  shared.target = *target;
  progress.stage = Stage::waiting_foreground;
  publish_progress(state);
  if (auto activated = window::activate(state, *target); !activated) {
    return activated;
  }
  if (auto started = capture::start(state, *found,
                                    {.width = target->width,
                                     .height = target->height,
                                     .comparison_rect = layout.stability_rect,
                                     .stable_seconds = stable_seconds});
      !started) {
    return started;
  }
  progress.stage = Stage::waiting_layout;
  publish_progress(state);
  return navigation::begin(state);
}

// 分类数组留在工作区，只有全部分类完成才交付完整结果。
auto run_all(core::AppState& state) -> std::expected<void, core::Error> {
  auto root = utils::file::executable_directory();
  if (!root) {
    return std::unexpected(root.error());
  }
  auto catalog = navigation::load(*root / "resources/categories");
  if (!catalog) {
    return std::unexpected(catalog.error());
  }
  auto& scan = *state.scan;
  auto& progress = scan.working_progress;
  for (const auto& group : catalog->groups) {
    progress.planned_categories += static_cast<int>(group.categories.size());
  }
  scan.resources.frame_timeout = frame_timeout;
  state.navigation->catalog = std::move(*catalog);
  state.navigation->timeout = frame_timeout;
  if (auto prepared = prepare_batch_capture(state); !prepared) {
    return prepared;
  }

  for (const auto& group : state.navigation->catalog.groups) {
    bool opened = false;
    for (const auto& icon : group.categories) {
      progress.category = icon.definition->id;
      progress.category_count = 0;
      progress.stage = Stage::switching_category;
      publish_progress(state);
      if (!opened) {
        if (auto result = navigation::open_group(state, group); !result) {
          return result;
        }
        opened = true;
      }
      if (auto result = navigation::visit(state, group, icon); !result) {
        return result;
      }
      scan.resources.category_frame = state.navigation->last_frame;
      progress.stage = Stage::collecting;
      publish_progress(state);
      if (auto result = collect_category(state); !result) {
        return result;
      }
      const auto& values = scan.category.values;
      scan.working_result.summaries.push_back({progress.category, static_cast<int>(values.size()),
                                               static_cast<int>(std::ranges::count(values, 1))});
      scan.completed.emplace(progress.category, std::move(scan.category.values));
      ++progress.completed_categories;
      progress.category_count = 0;
      publish_progress(state);
    }
  }
  progress.category.clear();
  progress.stage = Stage::completed;
  publish_progress(state);
  scan.working_result.status = Phase::completed;
  scan.working_result.categories = std::move(scan.completed);
  return {};
}

}  // namespace

auto running(const core::AppState& state) -> bool {
  return state.scan->phase == Phase::running || state.scan->phase == Phase::stopping;
}

auto start(core::AppState& state) -> void {
  // 任务已在运行或停止中则忽略重复请求
  if (running(state) || state.main_window->closing) {
    return;
  }
  state.scan->diagnostic.clear();
  auto root = utils::file::executable_directory();
  if (!root) {
    state.scan->phase = Phase::failed;
    state.scan->message = root.error();
    ui::main_window::refresh(state);
    return;
  }
  // 再次精确定位目标游戏主窗口
  auto game = features::window::find(nullptr, features::window::game_executable);
  if (!game) {
    state.scan->phase = Phase::failed;
    state.scan->message = game.error();
    ui::main_window::refresh(state);
    return;
  }
  state.window->game = *game;
  // 注册全局 F8 作为紧急中断热键
  if (!RegisterHotKey(state.main_window->hwnd, 1, MOD_NOREPEAT, VK_F8)) {
    ui::main_window::notify(state, core::error("error.app.register_hotkey"));
    return;
  }
  // 诊断根目录仅传入任务；异常收尾时才创建本次目录。
  features::scan::Request request;
  request.hwnd = state.window->game;
  request.diagnostic_root = *root / "data/diagnostics";
  // 跨线程唤醒回调：向主窗口投递自定义扫描进度消息
  state.scan->wake = [hwnd = state.main_window->hwnd] {
    PostMessageW(hwnd, ui::main_window::scan_update, 0, 0);
  };
  // 初始化运行期状态
  state.scan->phase = Phase::running;
  state.scan->progress = {};
  state.scan->message = {};
  state.main_window->notice = {};
  state.main_window->copied = false;
  state.scan->started = std::chrono::steady_clock::now();
  // 切回主界面并重置滚动位置
  state.main_window->page = ui::main_window::Page::main;
  state.main_window->scroll_y = 0;
  state.main_window->wheel_delta = 0;
  // 若悬浮窗此前发生过设备丢失，在此处重建资源状态
  if (state.status_window->graphics_failed) {
    state.status_window->buttons = {};
    state.status_window->indicator = {};
    state.status_window->composition = {};
    state.status_window->redraw = true;
    state.status_window->repaint_buttons = true;
  }
  state.status_window->graphics_failed = false;
  // 启动后台扫描工作线程
  if (auto started = job::start(state, std::move(request)); !started) {
    UnregisterHotKey(state.main_window->hwnd, 1);
    state.scan->phase = Phase::failed;
    state.scan->message = started.error();
  } else {
    // 成功启动后主窗口自动最小化，交由状态悬浮窗呈现进度
    ShowWindow(state.main_window->hwnd, SW_MINIMIZE);
  }
  ui::main_window::refresh(state);
}

auto stop(core::AppState& state) -> void {
  if (!running(state)) {
    return;
  }
  // 向扫描核心注入退出请求信号
  request_stop(state);
  state.scan->phase = Phase::stopping;
  ui::main_window::refresh(state);
  ui::status_window::refresh(state);
}

auto consume_updates(core::AppState& state) -> void {
  // 检查后台线程是否已完成并交付结果
  auto result = job::consume(state);
  if (result) {
    // 任务终结，注销 F8 热键
    UnregisterHotKey(state.main_window->hwnd, 1);
    state.scan->diagnostic = result->diagnostic;
    state.scan->message = result->message;
    // 成功、失败或停止都可能需要用户处理窗口恢复等收尾事项。
    if (!result->warning.message.key.empty()) {
      state.main_window->notice = result->warning;
      state.main_window->notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    }
    if (result->status == Phase::completed) {
      state.scan->phase = Phase::completed;
      state.result->last_success = std::move(*result);
    } else {
      state.scan->phase = result->status == Phase::stopped ? Phase::stopped : Phase::failed;
    }
    // 已经消费终态并回收线程，现在可以退出消息循环，交给应用 shutdown 销毁资源。
    if (state.main_window->closing) {
      ui::main_window::close(state);
      return;
    }
    // 扫描完成并收回悬浮窗
    ui::status_window::refresh(state, true);
  } else if (running(state)) {
    // 仍在进行中，刷新悬浮窗进度显示
    ui::status_window::refresh(state);
  }
  ui::main_window::refresh(state);
}

auto open_diagnostic(core::AppState& state) -> void {
  if (running(state)) {
    return;
  }
  std::error_code error;
  // 确保诊断目录存在且为有效文件夹
  if (state.scan->diagnostic.empty() ||
      !std::filesystem::is_directory(state.scan->diagnostic, error)) {
    ui::main_window::notify(state, core::error("error.app.no_diagnostics"));
    return;
  }
  // 通过 ShellExecuteW 调起资源管理器窗口
  auto result = ShellExecuteW(state.main_window->hwnd, L"open", state.scan->diagnostic.c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(result) <= 32) {
    ui::main_window::notify(state, core::error("error.app.open_diagnostics"));
  }
}

auto run(core::AppState& state) -> std::expected<Outcome, core::Error> {
  // 上次恢复失败时先处理原窗口，不覆盖恢复所需的信息。
  if (state.window->original) {
    if (auto restored = window::restore(state); !restored) {
      auto error = core::error("error.window.restore_failed");
      error.related.push_back(restored.error());
      return std::unexpected(std::move(error));
    }
  }
  std::expected<void, core::Error> result;
  try {
    result = run_all(state);
  } catch (const winrt::hresult_error& error) {
    result = std::unexpected(
        core::with_detail(core::error("error.scan.exception"), winrt::to_string(error.message())));
  } catch (const std::exception& error) {
    result = std::unexpected(core::with_detail(core::error("error.scan.exception"), error.what()));
  } catch (...) {
    result = std::unexpected(core::error("error.scan.exception"));
  }

  // 收尾失败不覆盖采集错误；完整结果已取得时保留数据并作为警告交付。
  auto add_cleanup_error = [&](core::Error error) {
    auto& outcome = state.scan->working_result;
    auto& target = outcome.warning;
    if (target.message.key.empty()) {
      target = std::move(error);
    } else {
      target.related.push_back(std::move(error));
    }
  };
  if (auto stopped = capture::stop(state); !stopped) {
    add_cleanup_error(stopped.error());
  }
  if (auto restored = window::restore(state); !restored) {
    // 窗口恢复需要用户处理，独立于扫描是否成功；优先于其它收尾警告展示。
    auto warning = core::error("error.window.restore_failed");
    warning.related.push_back(restored.error());
    auto& previous = state.scan->working_result.warning;
    if (!previous.message.key.empty()) {
      warning.related.push_back(std::move(previous));
    }
    previous = std::move(warning);
  }
  if (!result) {
    return std::unexpected(result.error());
  }
  return std::move(state.scan->working_result);
}

// 调用方已关闭捕获并保存诊断，此时才释放最后的业务现场。
auto release_run(core::AppState& state) -> void {
  state.scan->category = {};
  state.scan->resources = {};
  state.scan->completed.clear();
  *state.navigation = {};
}

}  // namespace features::scan

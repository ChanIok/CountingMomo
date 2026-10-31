#include "features/scan/usecase.hpp"

#include "vendor/std.hpp"

#include "vendor/windows/winrt/windows_foundation.hpp"

#include "core/message/message.hpp"

#include "core/state/app_state.hpp"
#include "features/capture/capture.hpp"
#include "features/navigation/navigation.hpp"
#include "features/result/result.hpp"
#include "features/scan/diagnostics.hpp"
#include "features/scan/layout.hpp"
#include "features/scan/progress.hpp"
#include "features/scan/state.hpp"
#include "features/scan/vision.hpp"
#include "features/window/state.hpp"
#include "features/window/window.hpp"
#include "utils/file.hpp"
#include "utils/image.hpp"
#include "utils/string.hpp"
#include "utils/timing.hpp"

namespace features::scan {

namespace {

using Clock = std::chrono::steady_clock;

struct ScrollPlan {
  int page_notches{};
  double pixels_per_notch{};
};

struct ScanResources {
  window::Target target;
  capture::Capture capture;
  Layout layout;
  cv::Mat hidden_question;
  ScrollPlan scroll;
  std::shared_ptr<const capture::Frame> category_frame;  // 导航／回顶确认帧，下一类首屏须晚于它。
  cv::Mat last_image;

  ~ScanResources() { capture::stop(capture); }
};

// 每个分类独立持有页面和诊断状态；窗口、WGC 与滚动标定由 ScanResources 跨类复用。
struct CategoryScan {
  std::filesystem::path diagnostic;
  Clock::time_point started{Clock::now()};
  Layout layout{};
  Session session;
  RunTimings timings;
  std::vector<int> values;
  cv::Mat last_image;
};

struct Observation {
  Page page;
  int displacement{};
  int advance{};  // 相对上一页推进的网格行数，不是像素数。
};

auto create_output_directory(const std::filesystem::path& path)
    -> std::expected<void, core::Error> {
  std::error_code ec;
  if (!std::filesystem::create_directories(path, ec)) {
    return std::unexpected(core::with_detail(
        core::error("error.scan.create_directory", utils::string::path_utf8(path)), ec.message()));
  }
  return {};
}

auto seconds(Clock::time_point time) -> double {
  return std::chrono::duration<double>(time.time_since_epoch()).count();
}

auto create_category_scan(std::string_view category, const std::filesystem::path& diagnostic)
    -> CategoryScan {
  CategoryScan scan;
  scan.diagnostic = diagnostic;
  scan.session.category = category;
  scan.session.created_at = utils::file::timestamp();
  return scan;
}

// 只接收输入基准之后的新源帧；接续和稳定确认完成前，不发送下一笔滚动。
auto wait_page(core::AppState& state, const Request& options, const ScanResources& resources,
               CategoryScan& scan, const Page* previous, const Anchor* anchor,
               Transaction& transaction) -> std::expected<Observation, core::Error> {
  const auto& layout = scan.layout;
  const auto& target = resources.target;
  auto deadline = Clock::now() + std::chrono::duration<double>(options.frame_timeout);
  auto last_sequence = transaction.before_sequence;
  auto last_source = transaction.before_source_time;
  std::optional<Page> base;
  Clock::time_point stable_since{};
  int base_displacement = -1;
  int stable_frames = 0;
  auto stage = core::message(previous ? "scan.wait.scroll_response" : "scan.wait.first_frame");
  std::optional<core::Error> last_error;
  while (Clock::now() < deadline) {
    if (auto ok = window::check(state, target); !ok) {
      return std::unexpected(ok.error());
    }
    auto snapshot = capture::snapshot(resources.capture);
    if (!snapshot) {
      return std::unexpected(snapshot.error());
    }
    auto frame = *snapshot;
    if (!frame || frame->sequence <= last_sequence || frame->source_time <= last_source) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    last_sequence = frame->sequence;
    last_source = frame->source_time;
    scan.last_image = frame->bgr;
    auto bottom = detect_bottom(frame->bgr, layout);
    TraceEntry entry{
        .sequence = frame->sequence,
        .source_time = frame->source_time,
        .received_seconds = seconds(frame->received_at),
        .at_bottom = bottom.at_bottom,
    };
    int displacement = 0;
    auto waiting = [&](core::Message reason) {
      last_error.reset();
      stage = std::move(reason);
      entry.stage = stage;
      transaction.trace.push_back(entry);
      base.reset();
      stable_frames = 0;
    };
    if (!previous && !items_at_top(frame->bgr, layout)) {
      waiting(core::message("scan.wait.checking_top"));
      continue;
    }
    if (previous) {
      auto match = locate_anchor(frame->bgr, *anchor, layout, bottom.at_bottom);
      entry.anchor = match;
      if (!match.valid) {
        waiting(core::message("scan.wait.anchor"));
        continue;
      }
      if (match.displacement < -1) {
        entry.stage = core::message("scan.wait.anchor_wrong_direction");
        transaction.trace.push_back(entry);
        return std::unexpected(core::error("error.scan.anchor_wrong_direction"));
      }
      displacement = std::max(0, match.displacement);
      if (displacement <= 1 && !bottom.at_bottom) {
        waiting(core::message("scan.wait.scroll_response"));
        continue;
      }
    }
    auto page = read_page(frame->bgr, layout, resources.hidden_question,
                          previous ? previous->phase - displacement : layout.first_row_y, !previous,
                          bottom);
    if (!page) {
      waiting(core::message("scan.wait.readable_cards"));
      last_error = page.error();
      transaction.trace.back().error = page.error();
      continue;
    }
    last_error.reset();
    entry.single_page = page->single_page;
    auto now = Clock::now();
    if (!base || base_displacement != displacement || !inputs_stable(*base, *page)) {
      base = *page;
      stable_since = now;
      stable_frames = 1;
      base_displacement = displacement;
    } else {
      ++stable_frames;
    }
    stage = core::message("scan.wait.stable");
    entry.displacement = displacement;
    entry.stable_frames = stable_frames;
    entry.stage = stage;
    bool ready = stable_frames >= 2 && std::chrono::duration<double>(now - stable_since).count() >=
                                           options.stable_seconds;
    if (ready) {
      entry.stage = core::message("scan.wait.ready");
    }
    transaction.trace.push_back(entry);
    if (!ready) {
      continue;
    }
    int advance = 0;
    if (previous) {
      double value = static_cast<double>(page->rows[0].y - previous->rows[0].y + displacement) /
                     layout.row_step;
      advance = static_cast<int>(std::round(value));
      if (advance < 0 || std::abs(value - advance) > 0.02) {
        return std::unexpected(core::error("error.scan.grid_displacement_mismatch"));
      }
    }
    transaction.actual_pixels = displacement;
    transaction.advance = advance;
    return Observation{std::move(*page), displacement, advance};
  }
  auto error = core::error("error.scan.sample_timeout");
  error.related.push_back({stage, {}, {}});
  if (last_error) {
    error.related.push_back(*last_error);
  }
  return std::unexpected(std::move(error));
}

auto append_page(std::vector<int>& values, const Page& page, int first_new) -> int {
  int before = static_cast<int>(values.size());
  for (int i = first_new; i < static_cast<int>(page.rows.size()); ++i) {
    for (const auto& card : page.rows[i].cards) {
      values.push_back(card.owned);
    }
  }
  return static_cast<int>(values.size()) - before;
}

auto observe_page(core::AppState& state, const Request& options, const ScanResources& resources,
                  CategoryScan& scan, std::ofstream& journal, const Page* previous,
                  const Anchor* anchor, Transaction& transaction)
    -> std::expected<Observation, core::Error> {
  auto observation = utils::timing::measure(scan.timings.page_wait_seconds, [&] {
    return wait_page(state, options, resources, scan, previous, anchor, transaction);
  });
  if (auto written = diagnostics::append(journal, transaction); !written) {
    if (!observation) {
      auto failure = observation.error();
      failure.related.push_back(written.error());
      return std::unexpected(std::move(failure));
    }
    return std::unexpected(written.error());
  }
  return observation;
}

auto accept_page(core::AppState& state, CategoryScan& scan, const Page& page, int first_new)
    -> void {
  int count = utils::timing::measure(scan.timings.append_seconds,
                                     [&] { return append_page(scan.values, page, first_new); });
  scan.session.accepted_count = static_cast<int>(scan.values.size());
  state.scan->working_progress.category_count = scan.session.accepted_count;
  publish_progress(state);
  diagnostics::log(core::message("log.scan.accepted", count, scan.session.accepted_count));
}

// 每类从已确认回顶的新源帧开始采集，跨页接续保留重叠区。
auto collect_pages(core::AppState& state, const Request& options, ScanResources& resources,
                   CategoryScan& scan) -> std::expected<void, core::Error> {
  auto& layout = scan.layout;
  auto& session = scan.session;
  const auto& target = resources.target;
  const auto& capture = resources.capture;
  std::ofstream journal(scan.diagnostic / "transactions.jsonl", std::ios::binary);
  if (!journal) {
    return std::unexpected(core::error("error.scan.transaction_open_failed"));
  }

  Transaction initial;
  if (resources.category_frame) {
    initial.before_sequence = resources.category_frame->sequence;
    initial.before_source_time = resources.category_frame->source_time;
  }
  auto observation =
      observe_page(state, options, resources, scan, journal, nullptr, nullptr, initial);
  if (!observation) {
    return std::unexpected(observation.error());
  }
  Page previous = std::move(observation->page);
  session.single_page = previous.single_page;
  if (previous.single_page) {
    layout.columns = layout.columns_without_scrollbar;
    diagnostics::log(core::message("log.scan.single_page"));
  }
  accept_page(state, scan, previous, 0);
  int accepted_rows = static_cast<int>(previous.rows.size());
  int previous_start = 0;
  auto& plan = resources.scroll;
  session.page_notches = plan.page_notches;
  session.pixels_per_notch = plan.pixels_per_notch;
  // 从可读高度扣除完整卡片和指定重叠行，统一确定自动标定的目标位移。
  int target_pixels = layout.safe_y[1] - layout.scrolled_safe_top - layout.card_size[1] -
                      options.overlap_rows * layout.row_step;
  for (int index = 1;; ++index) {
    if (previous.bottom.at_bottom || previous.single_page) {
      session.status = "completed";
      break;
    }
    if (target_pixels <= 0) {
      return std::unexpected(core::error("error.scan.insufficient_overlap"));
    }
    if (index > options.max_scrolls) {
      session.status = "scroll_limit";
      session.message = core::error("error.scan.scroll_limit");
      break;
    }
    if (auto ok = window::check(state, target); !ok) {
      return ok;
    }
    auto anchor = make_anchor(previous, layout, options.overlap_rows);
    if (!anchor) {
      return std::unexpected(anchor.error());
    }
    // 单屏及已回顶的分类不预先移动鼠标；只有实际滚动才解析滚动点。
    auto location = window::scroll_point(target, layout.scroll_rect);
    if (!location) {
      return std::unexpected(location.error());
    }
    auto baseline = capture::snapshot(capture);
    if (!baseline) {
      return std::unexpected(baseline.error());
    }
    if (!*baseline) {
      return std::unexpected(core::error("error.scan.baseline_lost"));
    }
    bool calibrating = plan.page_notches == 0;
    int notches = calibrating ? 6 : plan.page_notches;
    diagnostics::log(core::message(
        "log.scan.scroll", index, notches,
        core::text_key(calibrating ? "log.scan.calibrating" : "log.scan.fixed_scroll")));
    Transaction transaction;
    transaction.index = index;
    transaction.wheel_notches = notches;
    transaction.before_sequence = (*baseline)->sequence;
    transaction.before_source_time = (*baseline)->source_time;
    transaction.input_started_seconds = seconds(Clock::now());
    if (auto ok = window::scroll(state, target, *location, notches); !ok) {
      return ok;
    }
    transaction.input_sent_seconds = seconds(Clock::now());
    session.scroll_count = index;
    observation =
        observe_page(state, options, resources, scan, journal, &previous, &*anchor, transaction);
    if (!observation) {
      return std::unexpected(observation.error());
    }
    auto& current = observation->page;
    if (plan.pixels_per_notch > 0 && !current.bottom.at_bottom &&
        std::abs(observation->displacement - notches * plan.pixels_per_notch) > layout.row_step) {
      return std::unexpected(core::error("error.scan.calibration_mismatch"));
    }
    // 到底截短的位移不能用来标定；下一个多屏分类继续尝试首次有效标定。
    if (calibrating && !current.bottom.at_bottom) {
      if (observation->displacement <= 1) {
        return std::unexpected(core::error("error.scan.insufficient_calibration"));
      }
      plan.pixels_per_notch = observation->displacement / 6.0;
      plan.page_notches =
          std::clamp(static_cast<int>(std::floor(target_pixels / plan.pixels_per_notch)), 1, 273);
      session.pixels_per_notch = plan.pixels_per_notch;
      session.page_notches = plan.page_notches;
      diagnostics::log(core::message(
          "log.scan.calibrated", std::format("{:.2f}", plan.pixels_per_notch), plan.page_notches));
    }
    int current_start = previous_start + observation->advance;
    if (current_start > accepted_rows) {
      return std::unexpected(core::error("error.scan.uncovered_rows"));
    }
    int first_new = std::max(0, accepted_rows - current_start);
    accept_page(state, scan, current, first_new);
    accepted_rows = std::max(accepted_rows, current_start + static_cast<int>(current.rows.size()));
    previous = std::move(current);
    previous_start = current_start;
  }
  return {};
}

// 中断分类的前缀仅写诊断；完整结果由整个扫描成功后发布。
auto finish_category(core::AppState& state, CategoryScan& scan,
                     const std::expected<void, core::Error>& outcome)
    -> std::expected<void, core::Error> {
  auto& session = scan.session;
  auto& timings = scan.timings;
  if (!outcome) {
    session.status = window::stop_requested(state) ? "stopped" : "failed";
    session.message = outcome.error();
  }
  if (session.status == "failed" && !scan.last_image.empty()) {
    auto saved = utils::timing::measure(timings.diagnostic_save_seconds, [&] {
      return utils::image::save_png(scan.diagnostic / "unmatched_frame.png", scan.last_image);
    });
    if (!saved) {
      session.message.related.push_back(saved.error());
    }
  }
  auto exported =
      utils::timing::measure(timings.export_seconds, [&]() -> std::expected<void, core::Error> {
        if (session.status != "completed") {
          return result::write(scan.diagnostic / "partial.json", {{session.category, scan.values}});
        }
        return {};
      });
  if (!exported) {
    session.status = "failed";
    if (session.message.message.key.empty()) {
      session.message = exported.error();
    } else {
      session.message.related.push_back(exported.error());
    }
  }
  timings.wall_seconds = std::chrono::duration<double>(Clock::now() - scan.started).count();
  session.elapsed_seconds = timings.wall_seconds;
  auto state_saved = diagnostics::write(scan.diagnostic / "session.json", session);
  auto timing_saved = diagnostics::write(scan.diagnostic / "timings.json", timings);
  if (!state_saved || !timing_saved) {
    auto failure = session.message;
    for (const auto* saved : {&state_saved, &timing_saved}) {
      if (!*saved) {
        if (failure.message.key.empty()) {
          failure = saved->error();
        } else {
          failure.related.push_back(saved->error());
        }
      }
    }
    return std::unexpected(std::move(failure));
  }
  diagnostics::log(core::message("log.scan.category_finished", session.status,
                                 session.accepted_count,
                                 std::format("{:.3f}", timings.wall_seconds)));
  return {};
}

struct BatchRun {
  std::filesystem::path output;
  std::filesystem::path diagnostic;
  Clock::time_point started{Clock::now()};
  BatchTimings timings;
  BatchSession session;
  result::Categories completed;
};

auto save_batch_checkpoint(core::AppState& state, BatchRun& run)
    -> std::expected<void, core::Error> {
  auto& progress = state.scan->working_progress;
  progress.stage = run.session.stage;
  progress.completed_categories = run.session.completed_categories;
  progress.planned_categories = run.session.planned_categories;
  progress.accepted_count = run.session.accepted_count;
  publish_progress(state);
  run.session.elapsed_seconds = std::chrono::duration<double>(Clock::now() - run.started).count();
  return utils::timing::measure(run.timings.overview_export_seconds, [&] {
    return diagnostics::write(run.diagnostic / "session.json", run.session);
  });
}

auto prepare_batch_capture(core::AppState& state, const Request& options, ScanResources& shared,
                           navigation::Navigator& nav, BatchRun& run)
    -> std::expected<void, core::Error> {
  auto& batch = run.session;
  auto& timings = run.timings;
  if (auto ok = save_batch_checkpoint(state, run); !ok) {
    return ok;
  }
  {
    auto question = utils::timing::measure(timings.resource_load_seconds, [&] {
      return load_hidden_question(shared.layout, options.layout);
    });
    if (!question) {
      return std::unexpected(question.error());
    }
    shared.hidden_question = std::move(*question);
  }

  auto found = window::find(options.hwnd, options.exe_name);
  if (!found) {
    return std::unexpected(found.error());
  }
  batch.stage = Stage::adjusting_window;
  if (auto saved = save_batch_checkpoint(state, run); !saved) {
    return saved;
  }
  auto target = utils::timing::measure(timings.window_setup_seconds, [&] {
    return window::prepare(state, *found, shared.layout.reference_size[0],
                           shared.layout.reference_size[1], options.frame_timeout);
  });
  if (!target) {
    return std::unexpected(target.error());
  }
  shared.target = *target;
  nav.target = *target;
  batch.stage = Stage::waiting_foreground;
  if (auto saved = save_batch_checkpoint(state, run); !saved) {
    return saved;
  }
  if (auto ok = utils::timing::measure(timings.focus_wait_seconds,
                                       [&] { return window::activate(state, *target); });
      !ok) {
    return ok;
  }
  auto source = utils::timing::measure(timings.capture_start_seconds, [&] {
    return capture::start(*found, target->width, target->height);
  });
  if (!source) {
    return std::unexpected(source.error());
  }
  shared.capture = std::move(*source);
  batch.stage = Stage::waiting_layout;
  if (auto saved = save_batch_checkpoint(state, run); !saved) {
    return saved;
  }
  if (auto ok = utils::timing::measure(timings.navigation_seconds,
                                       [&] { return navigation::begin(state, nav); });
      !ok) {
    return ok;
  }
  return {};
}

// 已准备好的窗口和 WGC 由批量入口拥有；分类失败时立即停止捕获并保留前缀。
auto run_batch_category(core::AppState& state, const Request& options, ScanResources& resources,
                        CategoryScan& scan) -> std::expected<void, core::Error> {
  resources.last_image.release();
  if (auto created = create_output_directory(scan.diagnostic); !created) {
    return created;
  }
  scan.started = Clock::now();
  std::expected<void, core::Error> outcome;
  try {
    utils::timing::measure(scan.timings.resource_load_seconds, [&] {
      // 单屏列坐标、相位及锚点按分类重置；只有滚动标定跨分类保留。
      scan.layout = resources.layout;
    });
    auto target =
        utils::timing::measure(scan.timings.window_setup_seconds, [&] { return resources.target; });
    if (auto checked = utils::timing::measure(scan.timings.focus_wait_seconds,
                                              [&] { return window::check(state, target); });
        !checked) {
      outcome = std::unexpected(checked.error());
    } else {
      outcome = collect_pages(state, options, resources, scan);
    }
  } catch (const winrt::hresult_error& error) {
    outcome = std::unexpected(
        core::with_detail(core::error("error.scan.exception"), winrt::to_string(error.message())));
  } catch (const std::exception& error) {
    outcome = std::unexpected(core::with_detail(core::error("error.scan.exception"), error.what()));
  }
  if (!outcome || scan.session.status != "completed") {
    capture::stop(resources.capture);
  }
  resources.last_image = scan.last_image;
  return finish_category(state, scan, outcome);
}

auto scan_selected_category(core::AppState& state, const Request& options, ScanResources& shared,
                            navigation::Navigator& nav, BatchRun& run, BatchCategory& record,
                            int number) -> std::expected<void, core::Error> {
  auto& batch = run.session;
  auto& timings = run.timings;
  batch.stage = Stage::resetting_items;
  if (auto ok = save_batch_checkpoint(state, run); !ok) {
    return ok;
  }
  if (auto ok = utils::timing::measure(
          timings.list_reset_seconds,
          [&] { return navigation::reset_items_top(state, nav, shared.layout); });
      !ok) {
    return ok;
  }
  shared.category_frame = nav.last_frame;
  record.status = "scanning";
  batch.stage = Stage::collecting;
  if (auto ok = save_batch_checkpoint(state, run); !ok) {
    return ok;
  }
  auto relative = std::format("categories/{:02}_{}", number, record.id);
  auto scan = create_category_scan(record.id, run.diagnostic / utils::string::wide(relative));
  auto category_result = utils::timing::measure(timings.categories_wall_seconds, [&] {
    return run_batch_category(state, options, shared, scan);
  });
  const auto& category_session = scan.session;
  record.accepted_count = category_session.accepted_count;
  batch.accepted_count += record.accepted_count;
  record.message = category_result ? category_session.message : category_result.error();
  if (!category_result) {
    record.status = "failed";
    return std::unexpected(category_result.error());
  }
  if (category_session.status != "completed") {
    record.status = category_session.status;
    return std::unexpected(record.message);
  }
  run.completed.emplace(record.id, std::move(scan.values));
  record.status = "completed";
  state.scan->working_progress.category_count = 0;
  return {};
}

auto scan_all_categories(core::AppState& state, const Request& options,
                         const navigation::Catalog& catalog, ScanResources& shared,
                         navigation::Navigator& nav, BatchRun& run)
    -> std::expected<void, core::Error> {
  auto& batch = run.session;
  auto& timings = run.timings;
  if (auto prepared = prepare_batch_capture(state, options, shared, nav, run); !prepared) {
    return prepared;
  }
  batch.status = "running";
  std::size_t index = 0;
  int number = 0;
  for (const auto& group : catalog.groups) {
    bool opened = false;
    for (const auto& icon : group.categories) {
      auto& record = batch.categories[index++];
      ++number;
      batch.current_category = record.id;
      state.scan->working_progress.category = record.id;
      state.scan->working_progress.category_count = 0;
      batch.stage = Stage::switching_category;
      record.status = "navigating";
      if (auto ok = save_batch_checkpoint(state, run); !ok) {
        return ok;
      }
      auto navigated = utils::timing::measure(
          timings.navigation_seconds, [&]() -> std::expected<void, core::Error> {
            if (!opened) {
              if (auto ok = navigation::open_group(state, nav, group); !ok) {
                return ok;
              }
              opened = true;
            }
            return navigation::visit(state, nav, group, icon,
                                     std::format("{:02}_{}", number, record.id));
          });
      if (!navigated) {
        return std::unexpected(navigated.error());
      }
      diagnostics::log(core::message("log.scan.selected", number, batch.planned_categories,
                                     core::text_key("group." + record.group),
                                     core::text_key("category." + record.id)));
      if (auto scanned = scan_selected_category(state, options, shared, nav, run, record, number);
          !scanned) {
        return scanned;
      }
      ++batch.completed_categories;
      if (auto ok = save_batch_checkpoint(state, run); !ok) {
        return ok;
      }
    }
  }
  batch.status = "completed";
  batch.current_category.clear();
  batch.stage = Stage::completed;

  return {};
}

auto finish_batch(core::AppState& state, ScanResources& shared, navigation::Navigator& nav,
                  BatchRun& run, const std::expected<void, core::Error>& outcome)
    -> std::expected<void, core::Error> {
  auto& batch = run.session;
  auto& timings = run.timings;
  if (!outcome) {
    bool failed = std::ranges::any_of(
        batch.categories, [](const BatchCategory& record) { return record.status == "failed"; });
    bool limited = std::ranges::any_of(batch.categories, [](const BatchCategory& record) {
      return record.status == "scroll_limit";
    });
    batch.status = failed                          ? "failed"
                   : window::stop_requested(state) ? "stopped"
                   : limited                       ? "scroll_limit"
                                                   : "failed";
    batch.message = outcome.error();
    if (batch.stage == Stage::switching_category || batch.stage == Stage::waiting_layout) {
      batch.navigation_stage = nav.stage;
    }
    for (auto& record : batch.categories) {
      if (record.id == batch.current_category &&
          (record.status == "navigating" || record.status == "scanning")) {
        record.status = batch.status;
        record.message = batch.message;
      }
    }
    // 优先保存当前捕获帧；分类扫描已停止WGC时，下面使用该类最后的有效原图。
    if (shared.capture.state) {
      auto latest = capture::snapshot(shared.capture);
      if (latest && *latest) {
        nav.last_frame = *latest;
      }
    }
  }
  capture::stop(shared.capture);
  cv::Mat failure_image = nav.last_frame ? nav.last_frame->bgr : cv::Mat{};
  if (batch.stage == Stage::collecting && !shared.last_image.empty()) {
    failure_image = shared.last_image;
  }
  if (!outcome && !failure_image.empty()) {
    auto saved = utils::image::save_png(run.diagnostic / "unmatched_frame.png", failure_image);
    if (!saved) {
      batch.message.related.push_back(saved.error());
    }
  }
  if (auto ok = save_batch_checkpoint(state, run); !ok) {
    auto failure = batch.message;
    if (failure.message.key.empty()) {
      failure = ok.error();
    } else {
      failure.related.push_back(ok.error());
    }
    return std::unexpected(std::move(failure));
  }
  timings.wall_seconds = std::chrono::duration<double>(Clock::now() - run.started).count();
  if (auto ok = diagnostics::write(run.diagnostic / "timings.json", timings); !ok) {
    auto failure = batch.message;
    if (failure.message.key.empty()) {
      failure = ok.error();
    } else {
      failure.related.push_back(ok.error());
    }
    return std::unexpected(std::move(failure));
  }
  diagnostics::log(core::message("log.scan.finished", batch.status, batch.completed_categories,
                                 batch.planned_categories, batch.accepted_count,
                                 utils::string::path_utf8(run.output)));
  auto& result = state.scan->working_result;
  result.status = batch.status;
  result.message = batch.message;
  result.elapsed_seconds = timings.wall_seconds;
  for (const auto& record : batch.categories) {
    auto found = run.completed.find(record.id);
    if (found != run.completed.end()) {
      result.summaries.push_back({record.id, static_cast<int>(found->second.size()),
                                  static_cast<int>(std::ranges::count(found->second, 1))});
    }
  }
  if (batch.status == "completed") {
    result.categories = std::move(run.completed);
  }
  return {};
}

auto run_all(core::AppState& state, const Request& options) -> std::expected<void, core::Error> {
  auto layout = load_layout(options.layout);
  if (!layout) {
    return std::unexpected(layout.error());
  }
  auto catalog = navigation::load(options.categories);
  if (!catalog) {
    return std::unexpected(catalog.error());
  }
  if (layout->reference_size != catalog->settings.reference_size) {
    return std::unexpected(core::error("error.scan.layout_size_mismatch"));
  }
  const auto& output = options.output;
  state.scan->working_result.diagnostic = output / "diagnostics";
  if (auto created = create_output_directory(output); !created) {
    return std::unexpected(created.error());
  }
  auto diagnostic = output / "diagnostics";
  if (auto created = create_output_directory(diagnostic / "navigation"); !created) {
    return std::unexpected(created.error());
  }
  if (auto created = create_output_directory(diagnostic / "categories"); !created) {
    return std::unexpected(created.error());
  }

  BatchRun run{.output = output, .diagnostic = diagnostic};
  auto& batch = run.session;
  batch.created_at = utils::file::timestamp();
  for (const auto& group : catalog->groups) {
    for (const auto& icon : group.categories) {
      BatchCategory record{
          .id = icon.definition.id,
          .group = group.definition.id,
      };
      ++batch.planned_categories;
      batch.categories.push_back(std::move(record));
    }
  }

  ScanResources shared;
  shared.layout = *layout;
  navigation::Navigator nav{
      .catalog = &*catalog,
      .capture = &shared.capture,
      .output = diagnostic / "navigation",
      .timeout = options.frame_timeout,
  };
  std::expected<void, core::Error> outcome;
  try {
    outcome = scan_all_categories(state, options, *catalog, shared, nav, run);
  } catch (const winrt::hresult_error& error) {
    outcome = std::unexpected(
        core::with_detail(core::error("error.scan.exception"), winrt::to_string(error.message())));
  } catch (const std::exception& error) {
    outcome = std::unexpected(core::with_detail(core::error("error.scan.exception"), error.what()));
  }
  return finish_batch(state, shared, nav, run, outcome);
}

}  // namespace

auto run(core::AppState& state, const Request& request) -> std::expected<Outcome, core::Error> {
  state.scan->working_result = {};
  state.scan->working_progress = {};
  state.window->restore_enabled = request.restore_window;
  // 上次恢复失败时先处理原窗口，不覆盖恢复所需的信息。
  if (state.window->original) {
    if (auto restored = window::restore(state); !restored) {
      return std::unexpected(restored.error());
    }
  }
  std::expected<void, core::Error> result;
  try {
    result = run_all(state, request);
  } catch (const winrt::hresult_error& error) {
    result = std::unexpected(
        core::with_detail(core::error("error.scan.exception"), winrt::to_string(error.message())));
  } catch (const std::exception& error) {
    result = std::unexpected(core::with_detail(core::error("error.scan.exception"), error.what()));
  }
  auto restored = window::restore(state);
  if (!restored) {
    if (!result || state.scan->working_result.status != "completed") {
      auto error = result ? state.scan->working_result.message : result.error();
      error.related.push_back(restored.error());
      return std::unexpected(std::move(error));
    }
    // 结果已经完整采集，窗口恢复失败不丢弃可复制数据。
    state.scan->working_result.warning = restored.error();
  }
  if (!result) {
    return std::unexpected(result.error());
  }
  return std::move(state.scan->working_result);
}
}  // namespace features::scan

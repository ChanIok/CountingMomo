#include "features/scan/usecase.hpp"

#include "vendor/std.hpp"

#include "vendor/rfl.hpp"
#include "vendor/windows/combaseapi.hpp"
#include "vendor/windows/winrt/windows_foundation.hpp"

#include "features/capture/capture.hpp"
#include "features/recognition/recognition.hpp"
#include "features/report/report.hpp"
#include "features/scan/vision.hpp"
#include "features/window/window.hpp"
#include "utils/file.hpp"
#include "utils/image.hpp"
#include "utils/string.hpp"
#include "utils/timing.hpp"

namespace features::scan {
namespace {
using Clock = std::chrono::steady_clock;
struct PageJob {
  std::string frame_id;
  cv::Mat image;
  std::vector<Sample> samples;
};
struct WorkerTimings {
  double processing_seconds{};
  double idle_seconds{};
  double frame_save_seconds{};
  double ownership_seconds{};
  double journal_seconds{};
  double queue_wait_seconds{};
};
struct RunTimings {
  std::string units{"seconds"};
  std::string note{
      "各线程并行；识别分项包含于识别总计，模板按需生成也计入识别总计；"
      "采集识别阶段含启动准备；后台处理包含识别、页面存图和JSONL，不含报告阶段裁图；"
      "采样等待包含视觉检查，队列等待和排空等待与后台工作重叠，不能相加。"};
  double wall_seconds{};
  double font_load_seconds{};
  double window_setup_seconds{};
  double focus_wait_seconds{};
  double capture_start_seconds{};
  double page_wait_seconds{};
  double drain_wait_seconds{};
  double diagnostic_save_seconds{};
  double processing_wall_seconds{};
  double crop_export_seconds{};
  double report_seconds{};
  double template_generation_seconds{};
  int generated_template_banks{};
  double recognition_ms_per_name{};
  recognition::Timings recognition;
  WorkerTimings worker;
};
struct Worker {
  recognition::NameRecognizer recognizer;
  Layout layout;
  std::filesystem::path output;
  std::mutex mutex;
  std::condition_variable condition;
  std::deque<PageJob> queue;
  bool closing{};
  std::string error;
  std::vector<Item> items;
  WorkerTimings timings;
  std::jthread thread;
  ~Worker() {
    {
      std::scoped_lock lock(mutex);
      closing = true;
    }
    condition.notify_all();
    if (thread.joinable()) thread.join();
  }
};
auto worker_error(Worker& worker) -> std::string {
  std::scoped_lock lock(worker.mutex);
  return worker.error;
}
auto set_worker_error(Worker& worker, std::string text) -> void {
  std::scoped_lock lock(worker.mutex);
  if (worker.error.empty()) worker.error = std::move(text);
}
auto process_job(Worker& worker, PageJob job, std::ofstream& journal) -> void {
  utils::timing::Scope timer(worker.timings.processing_seconds);
  auto evidence = utils::timing::measure(worker.timings.frame_save_seconds, [&] {
    return utils::image::save_jpg(worker.output / "frames" / (job.frame_id + ".jpg"), job.image);
  });
  if (!evidence) set_worker_error(worker, evidence.error());
  for (auto& sample : job.samples) {
    auto& item = sample.item;
    try {
      utils::timing::measure(worker.timings.ownership_seconds,
                             [&] { ownership(item, sample.star, worker.layout.ownership_star); });
      if (worker_error(worker).empty()) {
        auto name = recognition::recognize(worker.recognizer, sample.name);
        if (name)
          item.name = std::move(*name);
        else
          set_worker_error(worker, name.error());
      }
    } catch (const std::exception& e) {
      set_worker_error(worker, e.what());
    } catch (const winrt::hresult_error& e) {
      set_worker_error(worker, winrt::to_string(e.message()));
    }
    if (auto error = worker_error(worker); !error.empty() && item.name.text.empty())
      item.name.issues.push_back("名称未完成识别：" + error);
    for (const auto& issue : item.name.issues) item.review_reasons.push_back("名称：" + issue);
    if (item.ownership == "unknown") item.review_reasons.push_back("拥有状态需要核对");
    item.needs_review = !item.review_reasons.empty();
    utils::timing::measure(worker.timings.journal_seconds,
                           [&] { journal << rfl::json::write(item) << '\n'; });
    worker.items.push_back(std::move(item));
    if (worker.items.size() % 16 == 0) {
      utils::timing::measure(worker.timings.journal_seconds, [&] { journal.flush(); });
      cli::log(std::format("已处理 {} 件。", worker.items.size()));
    }
    if (!journal) set_worker_error(worker, "items.jsonl 写入失败。");
  }
  utils::timing::measure(worker.timings.journal_seconds, [&] { journal.flush(); });
  cli::log(std::format("已处理 {} 件。", worker.items.size()));
}
auto start_worker(recognition::NameRecognizer recognizer, const Layout& layout,
                  const std::filesystem::path& output) -> std::unique_ptr<Worker> {
  auto worker = std::make_unique<Worker>();
  worker->recognizer = std::move(recognizer);
  worker->layout = layout;
  worker->output = output;
  worker->thread = std::jthread([w = worker.get()] {
    HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(apartment)) {
      set_worker_error(*w, "识别线程COM初始化失败。");
      return;
    }
    try {
      std::ofstream journal(w->output / "items.jsonl", std::ios::binary);
      if (!journal) set_worker_error(*w, "无法创建 items.jsonl。");
      while (true) {
        PageJob job;
        {
          std::unique_lock lock(w->mutex);
          utils::timing::measure(w->timings.idle_seconds, [&] {
            w->condition.wait(lock, [&] { return w->closing || !w->queue.empty(); });
          });
          if (w->queue.empty()) break;
          job = std::move(w->queue.front());
          w->queue.pop_front();
        }
        w->condition.notify_all();
        process_job(*w, std::move(job), journal);
      }
    } catch (const std::exception& e) {
      set_worker_error(*w, e.what());
    } catch (...) {
      set_worker_error(*w, "识别线程异常结束。");
    }
    CoUninitialize();
  });
  return worker;
}
auto check(const window::Target& target, Worker* worker, bool foreground = true)
    -> std::expected<void, std::string> {
  if (auto ok = window::check(target, foreground); !ok) return ok;
  if (worker)
    if (auto error = worker_error(*worker); !error.empty())
      return std::unexpected("识别线程：" + error);
  return {};
}
struct TraceEntry {
  std::uint64_t sequence{};
  std::int64_t source_time{};
  double received_seconds{};
  bool at_bottom{};
  bool single_page{};
  int displacement{};
  int stable_frames{};
  std::optional<AnchorMatch> anchor;
  std::string stage;
};
struct Transaction {
  int index{};
  int wheel_notches{};
  std::uint64_t before_sequence{};
  std::int64_t before_source_time{};
  double input_started_seconds{};
  double input_sent_seconds{};
  int actual_pixels{};
  int advance{};
  std::vector<TraceEntry> trace;
};
struct Observation {
  Page page;
  int displacement{};
  int advance{};
};
auto seconds(Clock::time_point time) -> double {
  return std::chrono::duration<double>(time.time_since_epoch()).count();
}
auto wait_page(const capture::Capture& capture, const Layout& layout, const Options& options,
               const window::Target& target, Worker* worker, const Page* previous,
               const Anchor* anchor, Transaction& transaction, cv::Mat& last_image)
    -> std::expected<Observation, std::string> {
  auto deadline = Clock::now() + std::chrono::duration<double>(options.frame_timeout);
  auto last_sequence = transaction.before_sequence;
  auto last_source = transaction.before_source_time;
  std::optional<Page> base;
  Clock::time_point stable_since{};
  int base_displacement = -1, stable_frames = 0;
  std::string stage = previous ? "等待滚动响应" : "等待首屏";
  while (Clock::now() < deadline) {
    if (auto ok = check(target, worker); !ok) return std::unexpected(ok.error());
    auto snapshot = capture::snapshot(capture);
    if (!snapshot) return std::unexpected(snapshot.error());
    auto frame = *snapshot;
    if (!frame || frame->sequence <= last_sequence || frame->source_time <= last_source) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    last_sequence = frame->sequence;
    last_source = frame->source_time;
    last_image = frame->bgr;
    auto bottom = detect_bottom(frame->bgr, layout);
    TraceEntry entry{frame->sequence, frame->source_time, seconds(frame->received_at),
                     bottom.at_bottom};
    int displacement = 0;
    auto waiting = [&](std::string reason) {
      stage = std::move(reason);
      entry.stage = stage;
      transaction.trace.push_back(entry);
      base.reset();
      stable_frames = 0;
    };
    if (previous) {
      auto match = locate_anchor(frame->bgr, *anchor, layout, bottom.at_bottom);
      entry.anchor = match;
      if (!match.valid) {
        waiting("等待名称锚点，尚不能确认接续");
        continue;
      }
      if (match.displacement < -1) {
        entry.stage = "名称锚点向下移动";
        transaction.trace.push_back(entry);
        return std::unexpected("名称锚点移动方向不符。");
      }
      displacement = std::max(0, match.displacement);
      if (displacement <= 1 && !bottom.at_bottom) {
        waiting("等待本次滚动生效，未追加输入");
        continue;
      }
    }
    auto page = read_page(frame->bgr, layout,
                          previous ? previous->phase - displacement : layout.first_row_y, !previous,
                          bottom);
    if (!page) {
      waiting(page.error());
      continue;
    }
    entry.single_page = page->single_page;
    auto now = Clock::now();
    if (!base || base_displacement != displacement || !inputs_stable(*base, *page)) {
      base = *page;
      stable_since = now;
      stable_frames = 1;
      base_displacement = displacement;
    } else
      ++stable_frames;
    stage = "等待名称与星形前景稳定";
    entry.displacement = displacement;
    entry.stable_frames = stable_frames;
    entry.stage = stage;
    bool ready = stable_frames >= 2 && std::chrono::duration<double>(now - stable_since).count() >=
                                           options.stable_seconds;
    if (ready) entry.stage = "就绪";
    transaction.trace.push_back(entry);
    if (!ready) continue;
    int advance = 0;
    if (previous) {
      double value = static_cast<double>(page->rows[0].y - previous->rows[0].y + displacement) /
                     layout.row_step;
      advance = static_cast<int>(std::round(value));
      if (advance < 0 || std::abs(value - advance) > 0.02)
        return std::unexpected("名称位移与网格不一致，停止以避免错误接续。");
    }
    transaction.actual_pixels = displacement;
    transaction.advance = advance;
    return Observation{std::move(*page), displacement, advance};
  }
  return std::unexpected("采样事务超时：" + stage + "；未发送后续滚动输入。");
}
auto submit(Worker& worker, PageJob job, const window::Target& target)
    -> std::expected<void, std::string> {
  while (true) {
    if (auto ok = check(target, &worker); !ok) return ok;
    std::unique_lock lock(worker.mutex);
    if (worker.queue.size() < 2) {
      worker.queue.push_back(std::move(job));
      lock.unlock();
      worker.condition.notify_all();
      return {};
    }
    utils::timing::measure(worker.timings.queue_wait_seconds,
                           [&] { worker.condition.wait_for(lock, std::chrono::milliseconds(50)); });
  }
}
auto page_job(const Page& page, int first_new, int global_start, const std::string& frame_id,
              const Options& options) -> PageJob {
  PageJob job{frame_id, page.image, {}};
  for (int i = first_new; i < static_cast<int>(page.rows.size()); ++i) {
    for (const auto& sample : page.rows[i].cards) {
      auto copy = sample;
      auto& item = copy.item;
      item.row = global_start + i - first_new + 1;
      item.id = std::format("r{:03}_c{:02}", item.row, item.column);
      item.category = options.category;
      item.frame_id = frame_id;
      item.card_crop = "crops/" + item.id + "_card.png";
      item.name_crop = "crops/" + item.id + "_name.png";
      item.star_crop = "crops/" + item.id + "_star.png";
      job.samples.push_back(std::move(copy));
    }
  }
  return job;
}
auto accept(Worker& worker, const Page& page, int first_new, int global_start,
            const std::string& frame_id, const Options& options, const window::Target& target)
    -> std::expected<int, std::string> {
  auto job = page_job(page, first_new, global_start, frame_id, options);
  int count = static_cast<int>(job.samples.size());
  if (count)
    if (auto ok = submit(worker, std::move(job), target); !ok) return std::unexpected(ok.error());
  return count;
}
}  // namespace
auto run(const Options& options) -> std::expected<int, std::string> {
  if (options.command == "list-windows") {
    for (const auto& w : window::list_windows())
      cli::log(std::format("0x{:x}\t{}\t{}\t{}", reinterpret_cast<std::uintptr_t>(w.hwnd), w.pid,
                           w.exe, w.title));
    return 0;
  }
  auto layout_result = load_layout(options.layout);
  if (!layout_result) return std::unexpected(layout_result.error());
  auto& layout = *layout_result;
  HWND hwnd{};
  if (options.command != "analyze") {
    auto found = window::find(options.hwnd, options.exe_name);
    if (!found) return std::unexpected(found.error());
    hwnd = *found;
  }
  auto output = options.output.empty()
                    ? std::filesystem::current_path() / "results" / utils::file::timestamp()
                    : options.output;
  std::error_code ec;
  if (!std::filesystem::create_directories(output, ec))
    return std::unexpected("结果目录已存在或无法创建：" + utils::string::path_utf8(output) + " " +
                           ec.message());
  for (auto folder : {"frames", "crops"}) {
    std::filesystem::create_directory(output / folder, ec);
    if (ec) return std::unexpected("无法创建证据目录：" + ec.message());
  }
  Session session;
  session.category = options.category;
  session.created_at = utils::file::timestamp();
  session.expected_count = options.expected_count;
  session.validate_characters = options.validate_characters;
  session.font = utils::string::path_utf8(std::filesystem::absolute(options.font));
  auto started = Clock::now();
  RunTimings timings;
  capture::Capture capture;
  std::unique_ptr<Worker> worker;
  cv::Mat last_image;
  auto checkpoint = [&]() {
    return utils::file::write(output / "session.json", rfl::json::write(session));
  };
  auto execute = [&]() -> std::expected<void, std::string> {
    if (options.command == "capture") {
      auto size = window::geometry(hwnd);
      if (!size) return std::unexpected(size.error());
      DWORD pid{};
      GetWindowThreadProcessId(hwnd, &pid);
      window::Target target{hwnd, pid, size->width, size->height};
      auto source = utils::timing::measure(timings.capture_start_seconds, [&] {
        return capture::start(hwnd, size->width, size->height);
      });
      if (!source) return std::unexpected(source.error());
      capture = std::move(*source);
      auto deadline = Clock::now() + std::chrono::duration<double>(options.frame_timeout);
      while (Clock::now() < deadline) {
        if (auto ok = window::check(target, false); !ok) return ok;
        auto frame = capture::snapshot(capture);
        if (!frame) return std::unexpected(frame.error());
        if (*frame) {
          last_image = (*frame)->bgr;
          if (auto ok = utils::image::save_png(output / "capture.png", last_image); !ok) return ok;
          session.status = "capture_only";
          session.message = "已保存当前客户区，未调整窗口或发送输入。";
          return {};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      return std::unexpected("等待首个WGC帧超时。");
    }
    auto recognizer = utils::timing::measure(timings.font_load_seconds, [&] {
      return recognition::load(options.font, options.names, layout, options.validate_characters);
    });
    if (!recognizer) return std::unexpected(recognizer.error());
    timings.template_generation_seconds = recognizer->font->generation_seconds;
    timings.generated_template_banks = recognizer->font->generated_banks;
    session.candidate_characters = static_cast<int>(recognizer->font->characters.size());
    session.names_file = utils::string::path_utf8(std::filesystem::absolute(options.names));
    session.candidate_names = static_cast<int>(recognizer->library.names.size());
    session.ignored_names = recognizer->library.ignored_names;
    for (const auto& name : session.ignored_names) cli::log("已排除不在扫描范围内的名称：" + name);
    if (recognizer->font->fallback_face) {
      session.fallback_font = utils::string::path_utf8(recognizer->font->fallback_path);
      for (char32_t cp : recognizer->font->fallback_characters) {
        auto text = utils::string::codepoint(cp);
        session.fallback_characters.push_back(text);
        cli::log("游戏字体缺字，使用系统微软雅黑：" + text);
      }
    }
    cli::log(std::format("名称库已就绪：{}个名称、{}个共享字符，普通与小字号使用统一识别。",
                         session.candidate_names, session.candidate_characters));
    cli::log(options.validate_characters ? "单字校验已开启，输出逐字证据并标记单字低分。"
                                         : "单字校验已关闭，仅按整名分数及候选差距接受名称。");
    if (options.command == "analyze") {
      session.source_image = utils::string::path_utf8(std::filesystem::absolute(options.image));
      auto image = utils::image::load(options.image);
      if (!image) return std::unexpected(image.error());
      last_image = std::move(*image);
      if (last_image.cols != layout.reference_size[0] ||
          last_image.rows != layout.reference_size[1])
        return std::unexpected("原图尺寸与布局不符；离线分析不缩放截图。");
      auto bottom = detect_bottom(last_image, layout);
      if (options.row_phase && *options.row_phase >= layout.row_step)
        return std::unexpected("行相位必须小于布局行距。");
      auto page = read_page(last_image, layout, options.row_phase.value_or(layout.first_row_y),
                            !options.row_phase.has_value(), bottom);
      if (!page) return std::unexpected(page.error());
      session.single_page = page->single_page;
      if (page->single_page) {
        layout.columns = layout.columns_without_scrollbar;
        cli::log("首屏未检测到滚动条，使用单屏列坐标读取当前分类。");
      }
      worker = start_worker(std::move(*recognizer), layout, output);
      auto job = page_job(*page, 0, 0, "f0001", options);
      session.accepted_count = static_cast<int>(job.samples.size());
      {
        std::scoped_lock lock(worker->mutex);
        worker->queue.push_back(std::move(job));
      }
      worker->condition.notify_all();
      session.status = "image_analyzed";
      session.message = "已分析单张截图的安全区域；不是完整分类扫描，行号仅为图内顺序。";
      return {};
    }
    auto prepared = utils::timing::measure(timings.window_setup_seconds, [&] {
      return window::prepare(hwnd, layout.reference_size[0], layout.reference_size[1],
                             options.frame_timeout);
    });
    if (!prepared) return std::unexpected(prepared.error());
    auto target = *prepared;
    if (auto ok = utils::timing::measure(timings.focus_wait_seconds,
                                         [&] { return window::activate(target); });
        !ok)
      return ok;
    auto point = [&]() -> std::expected<POINT, std::string> {
      if (options.scroll_point)
        return POINT{(*options.scroll_point)[0], (*options.scroll_point)[1]};
      return window::scroll_point(target, layout.scroll_rect);
    };
    auto initial_point = point();
    if (!initial_point) return std::unexpected(initial_point.error());
    if (auto ok = window::position_pointer(target, *initial_point); !ok) return ok;
    cli::log(std::format("滚动点（客户区）：({}, {})。", initial_point->x, initial_point->y));
    auto source = utils::timing::measure(timings.capture_start_seconds, [&] {
      return capture::start(hwnd, target.width, target.height);
    });
    if (!source) return std::unexpected(source.error());
    capture = std::move(*source);
    std::ofstream journal(output / "transactions.jsonl", std::ios::binary);
    if (!journal) return std::unexpected("无法创建事务诊断文件。");
    auto observe = [&](const Page* previous, const Anchor* anchor,
                       Transaction& transaction) -> std::expected<Observation, std::string> {
      auto result = utils::timing::measure(timings.page_wait_seconds, [&] {
        return wait_page(capture, layout, options, target, worker.get(), previous, anchor,
                         transaction, last_image);
      });
      journal << rfl::json::write(transaction) << '\n';
      journal.flush();
      if (!journal) return std::unexpected("事务记录写入失败。");
      return result;
    };
    Transaction initial;
    auto observation = observe(nullptr, nullptr, initial);
    if (!observation) return std::unexpected(observation.error());
    Page previous = std::move(observation->page);
    session.single_page = previous.single_page;
    if (previous.single_page) {
      layout.columns = layout.columns_without_scrollbar;
      cli::log("首屏未检测到滚动条，使用单屏列坐标；本次不发送滚动输入。");
    }
    worker = start_worker(std::move(*recognizer), layout, output);
    auto count = accept(*worker, previous, 0, 0, "f0001", options, target);
    if (!count) return std::unexpected(count.error());
    session.accepted_count = *count;
    cli::log(std::format("f0001 新增 {} 件，累计采集 {} 件。", *count, session.accepted_count));
    int accepted_rows = static_cast<int>(previous.rows.size()), previous_start = 0;
    int page_notches = options.wheel_notches;
    double rate = 0;
    int target_pixels = static_cast<int>(
        std::floor(std::min((layout.safe_y[1] - layout.scrolled_safe_top) * 0.8,
                            static_cast<double>(layout.safe_y[1] - layout.card_size[1] -
                                                layout.row_step - layout.scrolled_safe_top -
                                                (options.overlap_rows - 1) * layout.row_step))));
    for (int index = 1;; ++index) {
      if (options.expected_count && session.accepted_count > options.expected_count)
        return std::unexpected("采集数量超过指定总数。");
      if (previous.bottom.at_bottom || previous.single_page) {
        if (auto ok = utils::timing::measure(
                timings.diagnostic_save_seconds,
                [&] { return utils::image::save_png(output / "end_frame.png", previous.image); });
            !ok)
          return ok;
        if (options.expected_count && session.accepted_count != options.expected_count)
          return std::unexpected(std::format("分类采集结束，但采集 {}/{} 件，数量不符。",
                                             session.accepted_count, options.expected_count));
        session.status = options.expected_count ? "count_verified"
                         : previous.single_page ? "single_page_complete"
                                                : "bottom_reached";
        session.message = previous.single_page ? "当前分类已完整显示，单屏采集结束。"
                                               : "滚动条已到底，当前分类采集结束。";
        break;
      }
      if (target_pixels <= 0) return std::unexpected("安全区域不足以保留指定重叠行数。");
      if (index > options.max_scrolls) {
        session.status = "scroll_limit";
        session.message = "达到滚动次数上限，保存部分结果。";
        break;
      }
      if (auto ok = checkpoint(); !ok) return ok;
      if (auto ok = check(target, worker.get()); !ok) return ok;
      auto anchor = make_anchor(previous, layout, options.overlap_rows);
      if (!anchor) return std::unexpected(anchor.error());
      auto location = point();
      if (!location) return std::unexpected(location.error());
      auto baseline = capture::snapshot(capture);
      if (!baseline) return std::unexpected(baseline.error());
      if (!*baseline) return std::unexpected("捕获基准帧丢失。");
      bool calibrating = page_notches == 0;
      int notches = calibrating ? 6 : page_notches;
      cli::log(std::format("第 {} 次滚动：{} 格（{}），等待本次响应。", index, notches,
                           calibrating ? "小步标定" : "固定幅度"));
      Transaction transaction;
      transaction.index = index;
      transaction.wheel_notches = notches;
      transaction.before_sequence = (*baseline)->sequence;
      transaction.before_source_time = (*baseline)->source_time;
      transaction.input_started_seconds = seconds(Clock::now());
      if (auto ok = window::scroll(target, *location, notches); !ok) return ok;
      transaction.input_sent_seconds = seconds(Clock::now());
      session.scroll_count = index;
      observation = observe(&previous, &*anchor, transaction);
      if (!observation) return std::unexpected(observation.error());
      auto& current = observation->page;
      if (rate > 0 && !current.bottom.at_bottom &&
          std::abs(observation->displacement - notches * rate) > layout.row_step)
        return std::unexpected("滚动响应与一次标定不符，保存现场并停止。");
      if (calibrating && !current.bottom.at_bottom) {
        if (observation->displacement <= 1) return std::unexpected("小步滚动位移不足以完成标定。");
        rate = observation->displacement / 6.0;
        page_notches = std::clamp(static_cast<int>(std::floor(target_pixels / rate)), 1, 273);
        session.pixels_per_notch = rate;
        session.page_notches = page_notches;
        cli::log(std::format("标定完成：{:.2f}px/格，后续固定 {} 格，预计 {:.0f}px/次。", rate,
                             page_notches, rate * page_notches));
      }
      int current_start = previous_start + observation->advance;
      if (current_start > accepted_rows) return std::unexpected("相邻画面间存在未覆盖的行。");
      int first_new = std::max(0, accepted_rows - current_start);
      auto frame_id = std::format("f{:04}", index + 1);
      count =
          accept(*worker, current, first_new, current_start + first_new, frame_id, options, target);
      if (!count) return std::unexpected(count.error());
      session.accepted_count += *count;
      cli::log(
          std::format("{} 新增 {} 件，累计采集 {} 件。", frame_id, *count, session.accepted_count));
      int overlap = std::min(static_cast<int>(previous.rows.size()) - observation->advance,
                             static_cast<int>(current.rows.size()));
      cli::log(std::format("本次移动 {}px，接续重叠 {} 行。", observation->displacement, overlap));
      accepted_rows =
          std::max(accepted_rows, current_start + static_cast<int>(current.rows.size()));
      previous = std::move(current);
      previous_start = current_start;
    }
    return {};
  };
  std::expected<void, std::string> outcome;
  try {
    outcome = execute();
  } catch (const winrt::hresult_error& e) {
    outcome = std::unexpected(winrt::to_string(e.message()));
  } catch (const std::exception& e) {
    outcome = std::unexpected(e.what());
  }
  if (!outcome) {
    session.status = window::stop_requested() ? "stopped" : "failed";
    session.message = outcome.error();
  }
  capture::stop(capture);
  if (worker) {
    cli::log("采集已停止，等待已入队名称识别完成并导出。");
    {
      std::scoped_lock lock(worker->mutex);
      worker->closing = true;
    }
    worker->condition.notify_all();
    utils::timing::measure(timings.drain_wait_seconds, [&] { worker->thread.join(); });
    timings.worker = worker->timings;
    timings.recognition = worker->recognizer.timings;
    timings.template_generation_seconds = worker->recognizer.font->generation_seconds;
    timings.generated_template_banks = worker->recognizer.font->generated_banks;
    if (timings.recognition.names)
      timings.recognition_ms_per_name =
          1000 * timings.recognition.total_seconds / timings.recognition.names;
    session.processed_count = static_cast<int>(worker->items.size());
    auto error = worker_error(*worker);
    if (!error.empty() || session.processed_count != session.accepted_count) {
      session.status = "failed";
      session.message = error.empty() ? "已采集数量与处理数量不符。" : error;
    }
  }
  timings.processing_wall_seconds = std::chrono::duration<double>(Clock::now() - started).count();
  session.processing_seconds = timings.processing_wall_seconds;
  if (session.status == "failed" && !last_image.empty()) {
    if (auto ok = utils::timing::measure(
            timings.diagnostic_save_seconds,
            [&] { return utils::image::save_png(output / "unmatched_frame.png", last_image); });
        !ok)
      session.message += "；" + ok.error();
  }
  session.elapsed_seconds = std::chrono::duration<double>(Clock::now() - started).count();
  if (options.command != "capture") {
    cli::log(std::format("采集/识别阶段结束，用时 {:.3f}s；正在裁图并生成报告。",
                         timings.processing_wall_seconds));
    if (worker) {
      auto crops = utils::timing::measure(
          timings.crop_export_seconds, [&] { return report::save_crops(output, worker->items); });
      if (!crops) {
        session.status = "failed";
        session.message += "；报告裁图失败：" + crops.error();
      }
    }
    session.elapsed_seconds = std::chrono::duration<double>(Clock::now() - started).count();
    const std::vector<Item> empty;
    if (auto ok = utils::timing::measure(
            timings.report_seconds,
            [&] { return report::write(output, session, layout, worker ? worker->items : empty); });
        !ok)
      return std::unexpected(ok.error());
  }
  timings.wall_seconds = std::chrono::duration<double>(Clock::now() - started).count();
  session.elapsed_seconds = timings.wall_seconds;
  if (auto ok = checkpoint(); !ok) return std::unexpected(ok.error());
  if (auto ok = utils::file::write(output / "timings.json", rfl::json::write(timings)); !ok)
    return std::unexpected(ok.error());
  cli::log(std::format(
      "耗时统计（各项有包含和并行关系，不能直接相加）：\n"
      "  总耗时 {:.3f}s；采集/识别阶段 {:.3f}s；名称识别 {:.3f}s，平均 {:.2f}ms/名称\n"
      "  字体/名称库准备 {:.3f}s；模板生成 {:.3f}s（{}档，含初始及按需生成）\n"
      "  窗口设置 {:.3f}s；焦点等待 {:.3f}s；WGC启动 {:.3f}s\n"
      "  名称预处理 {:.3f}s；字号/起点几何 {:.3f}s；字格向量 {:.3f}s\n"
      "  灰度字形匹配 {:.3f}s；名称/布局评分 {:.3f}s；候选及证据 {:.3f}s\n"
      "  页面存图 {:.3f}s；报告阶段裁图/存图 {:.3f}s；拥有判定 {:.3f}s；JSONL {:.3f}s\n"
      "  采样等待/视觉 {:.3f}s；队列满等待 {:.3f}s；收尾排空等待 {:.3f}s\n"
      "  后台处理 {:.3f}s；后台空闲 {:.3f}s；诊断图 {:.3f}s；报告导出 {:.3f}s\n"
      "  名称 {} 条；查询位置 {} 个；模板比较 {} 次",
      timings.wall_seconds, timings.processing_wall_seconds, timings.recognition.total_seconds,
      timings.recognition_ms_per_name, timings.font_load_seconds,
      timings.template_generation_seconds, timings.generated_template_banks,
      timings.window_setup_seconds, timings.focus_wait_seconds, timings.capture_start_seconds,
      timings.recognition.preprocess_seconds, timings.recognition.geometry_seconds,
      timings.recognition.query_preparation_seconds, timings.recognition.template_match_seconds,
      timings.recognition.layout_selection_seconds, timings.recognition.candidate_seconds,
      timings.worker.frame_save_seconds, timings.crop_export_seconds,
      timings.worker.ownership_seconds, timings.worker.journal_seconds, timings.page_wait_seconds,
      timings.worker.queue_wait_seconds, timings.drain_wait_seconds,
      timings.worker.processing_seconds, timings.worker.idle_seconds,
      timings.diagnostic_save_seconds, timings.report_seconds, timings.recognition.names,
      timings.recognition.query_cells, timings.recognition.template_comparisons));
  cli::log(session.status + "：" + session.message);
  cli::log("结果目录：" + utils::string::path_utf8(std::filesystem::absolute(output)));
  return session.status == "failed"
             ? 1
             : (session.status == "stopped" || session.status == "scroll_limit" ? 2 : 0);
}
}  // namespace features::scan

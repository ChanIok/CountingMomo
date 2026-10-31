#include "features/scan/scan.hpp"

#include "vendor/std.hpp"

#include "core/message/message.hpp"
#include "core/state/app_state.hpp"
#include "features/capture/capture.hpp"
#include "features/scan/layout.hpp"
#include "features/scan/progress.hpp"
#include "features/scan/state.hpp"
#include "features/scan/vision.hpp"
#include "features/window/window.hpp"

namespace features::scan {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int overlap_rows = 4;  // 最多 3 行文字锚点，另留 1 行余量。
constexpr int max_scrolls = 1000;
constexpr int calibration_notches = 6;
constexpr double grid_row_tolerance = 0.02;

struct Observation {
  Page page;
  int displacement{};
  int advance{};  // 相对上一页推进的网格行数，不是像素数。
};

// 只解析 GPU 交付的稳定候选；未就绪则等画面变化后再判稳，不逐帧重复识别。
auto wait_page(core::AppState& state, const Page* previous, const Anchor* anchor,
               std::int64_t after_source_time) -> std::expected<Observation, core::Error> {
  auto& resources = state.scan->resources;
  auto& scan = state.scan->category;

  const auto& layout = get_layout();
  const auto& target = resources.target;
  auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                     std::chrono::duration<double>(resources.frame_timeout));
  auto initial = previous ? std::optional<capture::Frame>{} : resources.category_frame;
  bool require_change = false;
  auto stage = core::message(previous ? "scan.wait.scroll_response" : "scan.wait.first_frame");
  std::optional<core::Error> last_error;
  // 首屏复用分类切换后的稳定帧，翻页则等待输入完成后的稳定结果。
  while (Clock::now() < deadline) {
    if (auto checked = window::check(state, target); !checked) {
      return std::unexpected(checked.error());
    }
    std::expected<std::optional<capture::Frame>, core::Error> frame =
        initial ? std::expected<std::optional<capture::Frame>,
                                core::Error>{std::optional<capture::Frame>{std::move(*initial)}}
                : capture::wait_stable_frame(
                      state, after_source_time, deadline,
                      [&] { return window::check(state, target); }, require_change);
    initial.reset();
    if (!frame) {
      auto error = std::move(frame.error());
      error.related.push_back({stage, {}, {}});
      if (last_error) {
        error.related.push_back(*last_error);
      }
      return std::unexpected(std::move(error));
    }
    // 等到 deadline 仍无可解析的稳定帧，与循环结束共用同一个超时出口。
    if (!*frame) {
      break;
    }
    after_source_time = (**frame).source_time;
    require_change = true;
    scan.last_image = (**frame).bgr;
    scan.last_frame_source_time = (**frame).source_time;
    // 检测当前帧滚动条底部滑块状态
    auto bottom = detect_bottom((**frame).bgr);
    int displacement = 0;
    auto waiting = [&](core::Message reason) {
      last_error.reset();
      stage = std::move(reason);
    };
    // 非首屏通过上一页底部提取的文字锚点模板计算滚动位移
    if (previous) {
      auto match = locate_anchor((**frame).bgr, *anchor, layout, bottom.at_bottom);
      if (!match.valid) {
        waiting(core::message("scan.wait.anchor"));
        continue;
      }
      // 位移方向异常反向滚动则判定严重错误
      if (match.displacement < -1) {
        return std::unexpected(core::error("error.scan.anchor_wrong_direction"));
      }
      displacement = std::max(0, match.displacement);
      // 滚动后位移未产生实质推进且未触底时，继续等待后续帧渲染
      if (displacement <= 1 && !bottom.at_bottom) {
        waiting(core::message("scan.wait.scroll_response"));
        continue;
      }
    }
    // 按照滚动位移推算的相位，读取安全视野内的所有完整卡片状态
    auto page = read_page((**frame).bgr, layout, resources.hidden_question,
                          previous ? previous->phase - displacement : layout.first_row_y, !previous,
                          bottom);
    if (!page) {
      waiting(core::message("scan.wait.readable_cards"));
      last_error = page.error();
      continue;
    }
    last_error.reset();
    int advance = 0;
    // 换算网格逻辑推进的行数，必须严格吻合行高整数倍
    if (previous) {
      double value = static_cast<double>(page->rows[0].y - previous->rows[0].y + displacement) /
                     layout.row_step;
      advance = static_cast<int>(std::round(value));
      if (advance < 0 || std::abs(value - advance) > grid_row_tolerance) {
        return std::unexpected(core::error("error.scan.grid_displacement_mismatch"));
      }
    }
    return Observation{std::move(*page), displacement, advance};
  }
  // 超时未捕获到稳定合格帧，输出诊断错误
  auto error = core::error("error.scan.sample_timeout");
  error.related.push_back({stage, {}, {}});
  if (last_error) {
    error.related.push_back(*last_error);
  }
  return std::unexpected(std::move(error));
}

// 追加新识别的卡片拥有状态：跳过重叠行，按顺序追加 0/1 结果到结果向量
auto append_page(std::vector<int>& values, const Page& page, int first_new) -> void {
  // 仅遍历新进入安全视野的网格行
  for (int i = first_new; i < static_cast<int>(page.rows.size()); ++i) {
    for (const auto& card : page.rows[i].cards) {
      values.push_back(card.owned);
    }
  }
}

// 采纳并提交页面结果：追加结果集 → 统计当前分类累计数 → 发布进度通知
auto accept_page(core::AppState& state, const Page& page, int first_new) -> void {
  auto& scan = state.scan->category;

  append_page(scan.values, page, first_new);
  auto& progress = state.scan->working_progress;
  int count = static_cast<int>(scan.values.size());
  progress.accepted_count += count - progress.category_count;
  progress.category_count = count;
  // 向 UI 线程推送最新进度
  publish_progress(state);
}

// 分页循环采集单个分类全部卡片：首屏读取 → 提取重叠锚点 → 注入滚轮滚动 → 动态标定格数 →
// 拼接新行直到触底
auto collect_pages(core::AppState& state) -> std::expected<void, core::Error> {
  auto& resources = state.scan->resources;
  auto& scan = state.scan->category;

  const auto& layout = get_layout();
  const auto& target = resources.target;
  // 采集分类首屏画面
  auto observation =
      wait_page(state, nullptr, nullptr,
                resources.category_frame ? resources.category_frame->source_time : 0);
  if (!observation) {
    return std::unexpected(observation.error());
  }
  scan.previous = std::move(observation->page);
  auto& previous = scan.previous;
  // 采纳首屏全部卡片
  accept_page(state, previous, 0);
  auto& accepted_rows = scan.accepted_rows;
  auto& previous_start = scan.previous_start;
  accepted_rows = static_cast<int>(previous.rows.size());
  previous_start = 0;
  auto& plan = resources.scroll;
  // 从可读高度扣除完整卡片和指定重叠行，统一确定自动标定的目标位移。
  int target_pixels = layout.safe_y[1] - layout.scrolled_safe_top - layout.card_size[1] -
                      overlap_rows * layout.row_step;
  // 进入翻页滚动循环
  for (int index = 1;; ++index) {
    // 检测是否已触底或本分类原本就是单屏，完成采集
    if (previous.bottom.at_bottom || previous.single_page) {
      break;
    }
    if (target_pixels <= 0) {
      return std::unexpected(core::error("error.scan.insufficient_overlap"));
    }
    // 防止滚动陷入死循环
    if (index > max_scrolls) {
      return std::unexpected(core::error("error.scan.scroll_limit"));
    }
    // 校验前台窗口状态
    if (auto ok = window::check(state, target); !ok) {
      return ok;
    }
    // 从当前页底部抽取文字模板构造接续锚点
    auto anchor = make_anchor(previous, layout, overlap_rows);
    if (!anchor) {
      return std::unexpected(anchor.error());
    }
    // 计算右侧空白安全滚轮坐标
    auto location = window::scroll_point(target, layout.scroll_rect);
    if (!location) {
      return std::unexpected(location.error());
    }
    // 首次多屏滚动以小步输入标定像素比率，后续使用标定格数。
    bool calibrating = plan.page_notches == 0;
    int notches = calibrating ? calibration_notches : plan.page_notches;
    // 注入滚轮向下滚动输入
    if (auto ok = window::scroll(state, target, *location, notches); !ok) {
      return ok;
    }
    auto after_source_time = std::max(scan.last_frame_source_time, capture::source_time());
    // 等待滚动响应并解析新页
    observation = wait_page(state, &previous, &*anchor, after_source_time);
    if (!observation) {
      return std::unexpected(observation.error());
    }
    auto& current = observation->page;
    // 校验实际滚动位移与预估标定之间是否存在异常漂移
    if (plan.pixels_per_notch > 0 && !current.bottom.at_bottom &&
        std::abs(observation->displacement - notches * plan.pixels_per_notch) > layout.row_step) {
      return std::unexpected(core::error("error.scan.calibration_mismatch"));
    }
    // 若处于首次标定且未触底，计算每格滚轮对应像素位移并得出单页最佳步进格数
    if (calibrating && !current.bottom.at_bottom) {
      if (observation->displacement <= 1) {
        return std::unexpected(core::error("error.scan.insufficient_calibration"));
      }
      plan.pixels_per_notch = static_cast<double>(observation->displacement) / notches;
      plan.page_notches =
          std::clamp(static_cast<int>(std::floor(target_pixels / plan.pixels_per_notch)), 1,
                     window::max_wheel_notches);
    }
    // 计算当前页首行在全分类中的逻辑起始行号
    int current_start = previous_start + observation->advance;
    if (current_start > accepted_rows) {
      return std::unexpected(core::error("error.scan.uncovered_rows"));
    }
    // 仅采纳新露出来的后续行，剔除与上一页重叠的顶部旧行
    int first_new = std::max(0, accepted_rows - current_start);
    accept_page(state, current, first_new);
    accepted_rows = std::max(accepted_rows, current_start + static_cast<int>(current.rows.size()));
    previous = std::move(current);
    previous_start = current_start;
  }
  return {};
}

}  // namespace

auto collect_category(core::AppState& state) -> std::expected<void, core::Error> {
  state.scan->category = {};
  if (auto checked = window::check(state, state.scan->resources.target); !checked) {
    return checked;
  }
  return collect_pages(state);
}

}  // namespace features::scan

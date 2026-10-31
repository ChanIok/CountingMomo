#include "features/scan/diagnostics.hpp"

#include "vendor/std.hpp"

#include "vendor/rfl.hpp"

#include "core/message/message.hpp"
#include "core/state/app_state.hpp"
#include "features/navigation/state.hpp"
#include "features/scan/state.hpp"
#include "utils/file.hpp"
#include "utils/image.hpp"
#include "utils/string.hpp"

namespace features::scan::diagnostics {
namespace {
struct Report {
  std::string version{COUNTING_MOMO_VERSION};
  std::string build{__DATE__ " " __TIME__};
  std::string created_at;
  Phase status;
  Progress progress;
  core::Message navigation_stage;
  core::Error message;
  core::Error warning;
  double elapsed_seconds{};
  std::optional<core::Error> image_error;
};
}  // namespace

auto save(core::AppState& state, Outcome& outcome) -> std::expected<void, core::Error> {
  if (outcome.status != Phase::failed && outcome.warning.message.key.empty()) {
    return {};
  }
  auto created_at = utils::file::timestamp();
  auto directory = state.scan->request.diagnostic_root / created_at;
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    return std::unexpected(core::with_detail(
        core::error("error.scan.create_directory", utils::string::path_utf8(directory)),
        error.message()));
  }
  outcome.diagnostic = directory;

  // 导航可能已切到下一分类；按源时间选择最近的已观察图片，不在收尾重新等帧。
  const auto& category = state.scan->category;
  const auto& nav = *state.navigation;
  cv::Mat image = nav.last_frame ? nav.last_frame->bgr : cv::Mat{};
  if (!category.last_image.empty() &&
      (!nav.last_frame || category.last_frame_source_time >= nav.last_frame->source_time)) {
    image = category.last_image;
  }
  std::optional<core::Error> image_error;
  try {
    if (!image.empty()) {
      if (auto saved = utils::image::save_png(directory / "failure.png", image); !saved) {
        image_error = saved.error();
      }
    }
  } catch (const std::exception& exception) {
    image_error = core::with_detail(core::error("error.diagnostics.save_failed"), exception.what());
  }

  // 图片保存失败也继续写报告；原始扫描错误和收尾警告不被诊断错误替换。
  std::expected<void, core::Error> saved;
  try {
    Report report{
        .created_at = created_at,
        .status = outcome.status,
        .progress = state.scan->working_progress,
        .navigation_stage = nav.stage,
        .message = outcome.message,
        .warning = outcome.warning,
        .elapsed_seconds = outcome.elapsed_seconds,
        .image_error = image_error,
    };
    saved = utils::file::write(directory / "report.json", rfl::json::write(report));
  } catch (const std::exception& exception) {
    saved = std::unexpected(
        core::with_detail(core::error("error.diagnostics.save_failed"), exception.what()));
  }
  if (!saved) {
    if (image_error) {
      saved.error().related.push_back(*image_error);
    }
    return saved;
  }
  if (image_error) {
    return std::unexpected(*image_error);
  }
  return {};
}

}  // namespace features::scan::diagnostics

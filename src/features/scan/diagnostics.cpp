#include "features/scan/diagnostics.hpp"

#include "vendor/std.hpp"

#include "vendor/rfl.hpp"
#include "vendor/windows.hpp"

#include "core/message/message.hpp"

#include "utils/file.hpp"
#include "utils/string.hpp"

namespace features::scan::diagnostics {
auto log(const core::Message& message) -> void {
  auto text = utils::string::wide(rfl::json::write(message));
  text += L"\n";
  OutputDebugStringW(text.c_str());
}

namespace {
template <typename T>
auto save(const std::filesystem::path& path, const T& value) -> std::expected<void, core::Error> {
  try {
    return utils::file::write(path, rfl::json::write(value));
  } catch (const std::exception& error) {
    return std::unexpected(
        core::with_detail(core::error("error.diagnostics.serialize_failed"), error.what()));
  }
}
}  // namespace

auto append(std::ofstream& journal, const Transaction& transaction)
    -> std::expected<void, core::Error> {
  try {
    journal << rfl::json::write(transaction) << '\n';
  } catch (const std::exception& error) {
    return std::unexpected(
        core::with_detail(core::error("error.diagnostics.serialize_failed"), error.what()));
  }
  journal.flush();
  if (!journal) {
    return std::unexpected(core::error("error.diagnostics.write_failed"));
  }
  return {};
}

auto write(const std::filesystem::path& path, const Session& session)
    -> std::expected<void, core::Error> {
  return save(path, session);
}

auto write(const std::filesystem::path& path, const BatchSession& session)
    -> std::expected<void, core::Error> {
  return save(path, session);
}

auto write(const std::filesystem::path& path, const RunTimings& timings)
    -> std::expected<void, core::Error> {
  return save(path, timings);
}

auto write(const std::filesystem::path& path, const BatchTimings& timings)
    -> std::expected<void, core::Error> {
  return save(path, timings);
}

}  // namespace features::scan::diagnostics

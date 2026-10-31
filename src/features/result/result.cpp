#include "features/result/result.hpp"

#include "vendor/std.hpp"

#include "vendor/rfl.hpp"
#include "vendor/windows.hpp"

#include "core/message/message.hpp"

#include "utils/file.hpp"

namespace features::result {

auto serialize(const Categories& categories) -> std::expected<std::string, core::Error> {
  try {
    return rfl::json::write(categories);
  } catch (const std::exception& error) {
    return std::unexpected(
        core::with_detail(core::error("error.result.serialize_failed"), error.what()));
  }
}

auto write(const std::filesystem::path& path, const Categories& categories)
    -> std::expected<void, core::Error> {
  auto temporary = path;
  temporary += L".tmp";
  auto json = serialize(categories);
  if (!json) {
    return std::unexpected(json.error());
  }
  if (auto ok = utils::file::write(temporary, *json); !ok) {
    return ok;
  }
  if (!MoveFileExW(temporary.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return std::unexpected(core::error("error.result.replace_failed", GetLastError()));
  }
  return {};
}

}  // namespace features::result

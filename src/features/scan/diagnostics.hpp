#pragma once
#include "vendor/std.hpp"

#include "core/message/message.hpp"

#include "features/scan/diagnostic_types.hpp"

namespace features::scan::diagnostics {
auto log(const core::Message& message) -> void;
auto append(std::ofstream& journal, const Transaction& transaction)
    -> std::expected<void, core::Error>;
auto write(const std::filesystem::path& path, const Session& session)
    -> std::expected<void, core::Error>;
auto write(const std::filesystem::path& path, const BatchSession& session)
    -> std::expected<void, core::Error>;
auto write(const std::filesystem::path& path, const RunTimings& timings)
    -> std::expected<void, core::Error>;
auto write(const std::filesystem::path& path, const BatchTimings& timings)
    -> std::expected<void, core::Error>;
}  // namespace features::scan::diagnostics

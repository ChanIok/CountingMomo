#pragma once

#include "vendor/std.hpp"

#include "features/scan/types.hpp"

namespace features::report {
auto save_crops(const std::filesystem::path& output, const std::vector<scan::Item>& items)
    -> std::expected<void, std::string>;

auto write(const std::filesystem::path& output, const scan::Session& session,
           const scan::Layout& layout, const std::vector<scan::Item>& items)
    -> std::expected<void, std::string>;
}  // namespace features::report

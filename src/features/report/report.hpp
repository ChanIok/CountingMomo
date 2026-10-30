#pragma once

#include "vendor/std.hpp"

#include "features/scan/types.hpp"

namespace features::report {
auto save_crops(const std::filesystem::path& output, const std::vector<scan::Item>& items)
    -> std::expected<void, std::string>;

// 始终导出JSON/CSV；session.report_enabled决定是否另生成HTML。
auto write(const std::filesystem::path& output, const scan::Session& session,
           const scan::Layout& layout, const std::vector<scan::Item>& items)
    -> std::expected<void, std::string>;

// 始终保存批次状态；HTML总览同样由report_enabled控制。
auto write_batch(const std::filesystem::path& output, const scan::BatchSession& session)
    -> std::expected<void, std::string>;
}  // namespace features::report

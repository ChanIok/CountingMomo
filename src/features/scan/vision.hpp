#pragma once

#include "vendor/std.hpp"

#include "features/scan/types.hpp"

namespace features::scan {
auto detect_bottom(const cv::Mat& image, const Layout& layout) -> BottomResult;
auto read_page(const cv::Mat& image, const Layout& layout, int phase, bool at_top,
               BottomResult bottom) -> std::expected<Page, std::string>;
auto make_anchor(const Page& page, const Layout& layout, int overlap_rows)
    -> std::expected<Anchor, std::string>;
auto locate_anchor(const cv::Mat& image, const Anchor& anchor, const Layout& layout, bool at_bottom)
    -> AnchorMatch;
auto inputs_stable(const Page& previous, const Page& current) -> bool;
auto ownership(Item& item, const cv::Mat& star, const OwnershipSettings& settings) -> void;
}  // namespace features::scan

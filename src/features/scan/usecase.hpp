#pragma once

#include "vendor/std.hpp"

#include "cli.hpp"

namespace features::scan {
auto run(const Options& options) -> std::expected<int, std::string>;
}

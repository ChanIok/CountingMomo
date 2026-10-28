#pragma once

#include "vendor/std.hpp"

namespace utils::timing {
using Clock = std::chrono::steady_clock;
struct Scope {
  double& seconds;
  Clock::time_point started{Clock::now()};
  ~Scope() { seconds += std::chrono::duration<double>(Clock::now() - started).count(); }
  Scope(const Scope&) = delete;
  auto operator=(const Scope&) -> Scope& = delete;
  explicit Scope(double& destination) : seconds(destination) {}
};
template <typename Function>
auto measure(double& seconds, Function&& function) -> decltype(auto) {
  Scope timer(seconds);
  return std::forward<Function>(function)();
}
}  // namespace utils::timing

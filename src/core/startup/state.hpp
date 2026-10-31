#pragma once

#include "vendor/windows.hpp"

namespace core::startup {

struct State {
  HANDLE instance_mutex{};
};

}  // namespace core::startup

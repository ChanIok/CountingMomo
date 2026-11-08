#include "core/state/app_state.hpp"

#include "vendor/std.hpp"

#include "core/i18n/state.hpp"
#include "core/startup/state.hpp"
#include "features/result/state.hpp"
#include "features/scan/state.hpp"
#include "features/settings/state.hpp"
#include "features/window/state.hpp"
#include "ui/main_window/state.hpp"
#include "ui/shared_render_resources/state.hpp"
#include "ui/status_window/state.hpp"
#include "ui/theme/state.hpp"

namespace core {
// 初始化根应用状态聚合结构体：分配各子模块独立的状态独占指针 (unique_ptr)
AppState::AppState()
    : startup(std::make_unique<core::startup::State>()),
      settings(std::make_unique<features::settings::State>()),
      result(std::make_unique<features::result::State>()),
      i18n(std::make_unique<core::i18n::State>()),
      scan(std::make_unique<features::scan::State>()),
      window(std::make_unique<features::window::State>()),
      theme(std::make_unique<ui::theme::State>()),
      shared_render_resources(std::make_unique<ui::shared_render_resources::State>()),
      main_window(std::make_unique<ui::main_window::State>()),
      status_window(std::make_unique<ui::status_window::State>()) {}

AppState::~AppState() = default;
}  // namespace core

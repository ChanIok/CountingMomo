#pragma once

#include "vendor/std.hpp"

#include "core/i18n/types.hpp"
#include "core/message/message.hpp"

namespace core {
struct AppState;
}

namespace core::i18n {
auto system_language() -> Language;
auto locale(Language language) -> std::string_view;
auto current(const AppState& state) -> Language;
auto set_language(AppState& state, Language language) -> void;
auto text(Language language, const Message& message) -> std::string;
auto text(Language language, const Error& error) -> std::string;
auto wide(const AppState& state, const Message& message) -> std::wstring;
auto wide(const AppState& state, const Error& error) -> std::wstring;

template <typename... Args>
auto wide(const AppState& state, std::string_view key, const Args&... args) -> std::wstring {
  return wide(state, core::message(key, args...));
}
}  // namespace core::i18n

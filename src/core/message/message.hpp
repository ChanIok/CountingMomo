#pragma once
#include "vendor/std.hpp"

#include "core/message/types.hpp"

namespace core {
auto text_key(std::string key) -> MessageArgument;
auto argument(MessageArgument value) -> MessageArgument;

template <typename T>
auto argument(const T& value) -> MessageArgument {
  return {std::format("{}", value), false};
}

template <typename... Args>
auto message(std::string_view key, const Args&... args) -> Message {
  return {std::string(key), {argument(args)...}};
}

template <typename... Args>
auto error(std::string_view key, const Args&... args) -> Error {
  return {message(key, args...), {}, {}};
}

auto with_detail(Error error, std::string detail) -> Error;
}  // namespace core

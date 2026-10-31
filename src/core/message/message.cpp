#include "core/message/message.hpp"

#include "vendor/std.hpp"

namespace core {
auto text_key(std::string key) -> MessageArgument { return {std::move(key), true}; }

auto argument(MessageArgument value) -> MessageArgument { return value; }

auto with_detail(Error error, std::string detail) -> Error {
  error.detail = std::move(detail);
  return error;
}
}  // namespace core

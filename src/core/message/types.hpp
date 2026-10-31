#pragma once

#include "vendor/std.hpp"

namespace core {
struct MessageArgument {
  std::string value;
  bool is_key{};
};

struct Message {
  std::string key;
  std::vector<MessageArgument> args;
};

struct Error {
  Message message;
  std::string detail;          // 系统或第三方的原始诊断，不作为翻译 key。
  std::vector<Error> related;  // 内部上下文、下层原因或附加失败，不覆盖主错误。
};
}  // namespace core

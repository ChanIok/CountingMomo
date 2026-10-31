#include "core/message/message.hpp"

#include "vendor/std.hpp"

namespace core {
// 构造可供多语言翻译解析的文本 key 参数
auto text_key(std::string key) -> MessageArgument { return {std::move(key), true}; }

// 转发已构造成型的消息参数
auto argument(MessageArgument value) -> MessageArgument { return value; }

// 为结构化错误附加上下文原生详细诊断文本（如底层异常消息或 Win32 描述）
auto with_detail(Error error, std::string detail) -> Error {
  error.detail = std::move(detail);
  return error;
}
}  // namespace core

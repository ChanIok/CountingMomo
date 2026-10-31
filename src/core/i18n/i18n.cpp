#include "core/i18n/i18n.hpp"

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

#include "core/i18n/generated/texts.hpp"
#include "core/i18n/state.hpp"
#include "core/state/app_state.hpp"
#include "utils/string.hpp"

namespace core::i18n {
namespace {
// 在代码生成的多语言只读表中二分查找词条
auto lookup(Language language, std::string_view key) -> std::string_view {
  // 二分查找匹配 key
  auto found = std::ranges::lower_bound(generated::texts, key, {}, &generated::Text::key);
  // 未知 key 是开发错误；保留原标识，便于报告，不回退到另一种语言。
  if (found == generated::texts.end() || found->key != key) {
    return key;
  }
  return language == Language::zh_cn ? found->zh_cn : found->en_us;
}
}  // namespace

// 根据 Windows 用户默认 UI 语言决定默认多语言选项
auto system_language() -> Language {
  return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE ? Language::zh_cn
                                                                   : Language::en_us;
}

// 获取多语言对应的 BCP-47 标准代码（zh-CN 或 en-US）
auto locale(Language language) -> std::string_view {
  return language == Language::zh_cn ? "zh-CN" : "en-US";
}

// 读取当前配置的应用语言
auto current(const AppState& state) -> Language { return state.i18n->language; }

// 切换当前应用界面显示语言
auto set_language(AppState& state, Language language) -> void { state.i18n->language = language; }

// 格式化多语言消息：查表获取模板 → 替换 {0}、{1} 等编号参数
auto text(Language language, const Message& message) -> std::string {
  auto pattern = lookup(language, message.key);
  std::string result;
  // 只支持编号占位符 {0}、{1}，参数中的花括号不会被再次解释。
  for (std::size_t i = 0; i < pattern.size();) {
    if (pattern[i] == '{') {
      auto end = pattern.find('}', i + 1);
      if (end != std::string_view::npos) {
        std::size_t index = 0;
        bool valid = end > i + 1;
        // 解析花括号内部的索引数值
        for (auto j = i + 1; j < end; ++j) {
          valid = valid && pattern[j] >= '0' && pattern[j] <= '9';
          if (valid) {
            index = index * 10 + static_cast<std::size_t>(pattern[j] - '0');
          }
        }
        // 参数合法且未越界时，替换对应参数值（支持嵌套 key 递归翻译）
        if (valid && index < message.args.size()) {
          const auto& arg = message.args[index];
          result += arg.is_key ? lookup(language, arg.value) : std::string_view(arg.value);
          i = end + 1;
          continue;
        }
      }
    }
    result += pattern[i++];
  }
  return result;
}

// 格式化错误描述并附加 detail 及级联的 related 错误文本
auto text(Language language, const Error& error) -> std::string {
  // 解析主错误消息
  auto result = text(language, error.message);
  // 追加原生详情
  if (!error.detail.empty()) {
    result += "\n" + error.detail;
  }
  // 逐条递归追加关联与级联引发的附加错误
  for (const auto& related : error.related) {
    auto detail = text(language, related);
    if (!detail.empty()) {
      result += (result.empty() ? "" : "\n") + detail;
    }
  }
  return result;
}

// 将多语言消息格式化为供 Win32 / DirectWrite 使用的宽字符文本
auto wide(const AppState& state, const Message& message) -> std::wstring {
  return utils::string::wide(text(current(state), message));
}

// 将领域错误格式化为供 Win32 / DirectWrite 使用的宽字符文本
auto wide(const AppState& state, const Error& error) -> std::wstring {
  return utils::string::wide(text(current(state), error));
}
}  // namespace core::i18n

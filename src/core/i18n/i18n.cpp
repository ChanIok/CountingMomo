#include "core/i18n/i18n.hpp"

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

#include "core/i18n/generated/texts.hpp"
#include "core/i18n/state.hpp"
#include "core/state/app_state.hpp"
#include "utils/string.hpp"

namespace core::i18n {
namespace {
auto lookup(Language language, std::string_view key) -> std::string_view {
  auto found = std::ranges::lower_bound(generated::texts, key, {}, &generated::Text::key);
  // 未知 key 是开发错误；保留原标识，便于报告，不回退到另一种语言。
  if (found == generated::texts.end() || found->key != key) {
    return key;
  }
  return language == Language::zh_cn ? found->zh_cn : found->en_us;
}
}  // namespace

auto system_language() -> Language {
  return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE ? Language::zh_cn
                                                                   : Language::en_us;
}

auto locale(Language language) -> std::string_view {
  return language == Language::zh_cn ? "zh-CN" : "en-US";
}

auto current(const AppState& state) -> Language { return state.i18n->language; }

auto set_language(AppState& state, Language language) -> void { state.i18n->language = language; }

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
        for (auto j = i + 1; j < end; ++j) {
          valid = valid && pattern[j] >= '0' && pattern[j] <= '9';
          if (valid) {
            index = index * 10 + static_cast<std::size_t>(pattern[j] - '0');
          }
        }
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

auto text(Language language, const Error& error) -> std::string {
  auto result = text(language, error.message);
  if (!error.detail.empty()) {
    result += "\n" + error.detail;
  }
  for (const auto& related : error.related) {
    auto detail = text(language, related);
    if (!detail.empty()) {
      result += (result.empty() ? "" : "\n") + detail;
    }
  }
  return result;
}

auto wide(const AppState& state, const Message& message) -> std::wstring {
  return utils::string::wide(text(current(state), message));
}

auto wide(const AppState& state, const Error& error) -> std::wstring {
  return utils::string::wide(text(current(state), error));
}
}  // namespace core::i18n

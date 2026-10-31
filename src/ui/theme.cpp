#include "ui/theme.hpp"

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

#include "core/message/message.hpp"
#include "core/state/app_state.hpp"
#include "features/settings/state.hpp"
#include "ui/theme/state.hpp"

namespace ui::theme {
namespace {
// 将 GDI 的 COLORREF 颜色转换为 Direct2D 浮点颜色结构体
auto color(COLORREF value) -> D2D1_COLOR_F {
  return D2D1::ColorF(GetRValue(value) / 255.0f, GetGValue(value) / 255.0f,
                      GetBValue(value) / 255.0f);
}

// 从注册表中读取 Windows 个性化主题的 DWORD 配置值（如 AppsUseLightTheme 等）
auto setting(const wchar_t* key, DWORD default_value) -> DWORD {
  DWORD size = sizeof(DWORD);
  DWORD value = default_value;
  RegGetValueW(HKEY_CURRENT_USER,
               L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", key,
               RRF_RT_REG_DWORD, nullptr, &value, &size);
  return value;
}

// 计算颜色的相对亮度：将 sRGB 分量线性化后按标准光度权重加权求和
auto luminance(D2D1_COLOR_F value) -> double {
  auto linear = [](float channel) -> double {
    return channel <= 0.04045f ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * linear(value.r) + 0.7152 * linear(value.g) + 0.0722 * linear(value.b);
}

// 计算两种颜色之间的 WCAG 相对对比度比率
auto contrast(D2D1_COLOR_F first, D2D1_COLOR_F second) -> double {
  double first_luminance = luminance(first);
  double second_luminance = luminance(second);
  return (std::max(first_luminance, second_luminance) + 0.05) /
         (std::min(first_luminance, second_luminance) + 0.05);
}

// 计算并提取符合可读性对比度的系统主题色及符合 Fluent Design 的悬停/按压光感层
auto accent(Palette& colors, const winrt::Windows::UI::ViewManagement::UISettings& settings)
    -> void {
  using winrt::Windows::UI::ViewManagement::UIColorType;
  constexpr std::array types{UIColorType::AccentDark3,  UIColorType::AccentDark2,
                             UIColorType::AccentDark1,  UIColorType::Accent,
                             UIColorType::AccentLight1, UIColorType::AccentLight2,
                             UIColorType::AccentLight3};
  std::array<D2D1_COLOR_F, types.size()> shades;
  // 获取强调色的各级明暗变体
  for (std::size_t index = 0; index < types.size(); ++index) {
    auto value = settings.GetColorValue(types[index]);
    shades[index] = D2D1::ColorF(value.R / 255.0f, value.G / 255.0f, value.B / 255.0f);
  }
  // WinUI 3 规范：实心按钮浅色模式采用 AccentDark1 确保白字对比，深色模式采用 AccentLight2。
  int base = colors.dark ? 5 : 2;
  colors.accent = shades[base];
  auto black = D2D1::ColorF(0x000000);
  auto white = D2D1::ColorF(0xFFFFFF);
  // 基于 WCAG 对比度公式选择对比更清晰的强调文字颜色
  colors.accent_text =
      contrast(colors.accent, black) >= contrast(colors.accent, white) ? black : white;
  // 浅色模式若所选 Accent 对文字对比度不足，向深色方向收敛到可读色阶。
  while (!colors.dark && base > 0 && contrast(shades[base], colors.accent_text) < 4.5) {
    --base;
    colors.accent = shades[base];
  }
  // 对齐 WinUI 3 AccentFillColorSecondary/Tertiary：采用微光与微暗半透明图层遮罩。
  colors.accent_hover = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f);
  colors.accent_pressed = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.16f);
}
}  // namespace

// 初始化主题系统：创建 WinRT UISettings 实例并初次解析系统主题调色板
auto initialize(core::AppState& state) -> std::expected<void, core::Error> {
  try {
    state.theme->settings = winrt::Windows::UI::ViewManagement::UISettings();
    return refresh(state);
  } catch (const winrt::hresult_error& error) {
    return std::unexpected(core::with_detail(core::error("error.theme.initialize_failed"),
                                             winrt::to_string(error.message())));
  }
}

// 注册系统主题色彩变动的事件监听：向目标窗口投递更新通知消息
auto watch(core::AppState& state, HWND target, UINT message) -> std::expected<void, core::Error> {
  // 建立线程安全的通知承载结构
  auto notification = std::make_shared<Notification>();
  notification->target = target;
  notification->message = message;
  try {
    // 监听 Windows 主题色更新事件，变动时向 UI 消息队列投递通知
    state.theme->changed = state.theme->settings.ColorValuesChanged(
        winrt::auto_revoke, [notification](const auto&, const auto&) {
          std::lock_guard lock(notification->mutex);
          if (notification->target) {
            PostMessageW(notification->target, notification->message, 0, 0);
          }
        });
    state.theme->notification = std::move(notification);
  } catch (const winrt::hresult_error& error) {
    return std::unexpected(core::with_detail(core::error("error.theme.watch_failed"),
                                             winrt::to_string(error.message())));
  }
  // 订阅后再刷新，覆盖初始化读取与订阅建立之间的系统颜色变化。
  if (!PostMessageW(target, message, 0, 0)) {
    auto error = GetLastError();
    shutdown(state);
    return std::unexpected(core::error("error.theme.notify_failed", error));
  }
  return {};
}

// 注销并清理主题事件监听：断开目标窗口句柄并撤销 WinRT 监听令牌
auto shutdown(core::AppState& state) -> void {
  auto& theme = *state.theme;
  if (theme.notification) {
    std::lock_guard lock(theme.notification->mutex);
    theme.notification->target = nullptr;
  }
  // 撤销事件监听
  theme.changed.revoke();
  theme.notification.reset();
}

// 重新解析当前调色板：结合用户设置、深浅色模式与高对比度辅助选项构建完整配色方案
auto refresh(core::AppState& state) -> std::expected<void, core::Error> {
  try {
    // 判定最终深色/浅色显示形态
    auto mode = state.settings->values.theme;
    bool dark = mode == "dark" || (mode == "system" && setting(L"AppsUseLightTheme", 1) == 0);
    // 检测系统高对比度模式开启状态
    HIGHCONTRASTW contrast{sizeof(HIGHCONTRASTW)};
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
    bool high_contrast = (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
    // 组装常规明暗模式基础色板
    Palette result{
        .dark = dark,
        .high_contrast = high_contrast,
        .transparency = !high_contrast && setting(L"EnableTransparency", 1) != 0,
        .background = D2D1::ColorF(dark ? 0x191919 : 0xEFEFEF),
        .card = D2D1::ColorF(dark ? 0x272727 : 0xFFFFFF),
        .text = D2D1::ColorF(dark ? 0xECECEC : 0x1B1B1B),
        .muted = D2D1::ColorF(dark ? 0x9D9D9D : 0x6B6B6B),
        .line = D2D1::ColorF(dark ? 0x3D3D3D : 0xE5E5E5),
        .secondary = D2D1::ColorF(dark ? 0x373737 : 0xFFFFFF),
        .secondary_hover = D2D1::ColorF(dark ? 0x3F3F3F : 0xF7F7F7),
        .secondary_pressed = D2D1::ColorF(dark ? 0x323232 : 0xEEEEEE),
        .secondary_border = D2D1::ColorF(dark ? 0x606060 : 0xD1D1D1),
        .secondary_text = D2D1::ColorF(dark ? 0xECECEC : 0x1B1B1B),
        .soft = D2D1::ColorF(dark ? 0x313131 : 0xF2F2F2),
        .soft_hover =
            dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.05f),
        .soft_pressed =
            dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.09f),
        .danger = D2D1::ColorF(dark ? 0xF85149 : 0xC72E0F),
    };
    // 针对高对比度系统辅助选项覆盖指定系统颜色
    if (high_contrast) {
      result.background = color(GetSysColor(COLOR_WINDOW));
      result.card = result.background;
      result.text = color(GetSysColor(COLOR_WINDOWTEXT));
      result.muted = result.text;
      result.line = result.text;
      result.accent = color(GetSysColor(COLOR_HIGHLIGHT));
      result.accent_hover = result.accent;
      result.accent_pressed = result.accent;
      result.accent_text = color(GetSysColor(COLOR_HIGHLIGHTTEXT));
      result.secondary = color(GetSysColor(COLOR_BTNFACE));
      result.secondary_hover = result.secondary;
      result.secondary_pressed = result.secondary;
      result.secondary_border = color(GetSysColor(COLOR_BTNTEXT));
      result.secondary_text = result.secondary_border;
      result.soft = color(GetSysColor(COLOR_BTNFACE));
      result.soft_hover = result.soft;
      result.soft_pressed = result.soft;
      result.danger = result.text;
    } else {
      // 提取计算系统强调色及其变体
      accent(result, state.theme->settings);
    }
    state.theme->colors = result;
    return {};
  } catch (const winrt::hresult_error& error) {
    return std::unexpected(core::with_detail(core::error("error.theme.read_failed"),
                                             winrt::to_string(error.message())));
  }
}

// 获取当前已生效的主题调色板常量引用
auto resolve(const core::AppState& state) -> const Palette& { return state.theme->colors; }
}  // namespace ui::theme

#include "ui/theme.hpp"

#include "vendor/std.hpp"

#include "vendor/windows.hpp"

#include "core/message/message.hpp"

#include "app/state.hpp"
#include "core/state/app_state.hpp"
#include "ui/theme/state.hpp"

namespace ui::theme {
namespace {
auto color(COLORREF value) -> D2D1_COLOR_F {
  return D2D1::ColorF(GetRValue(value) / 255.0f, GetGValue(value) / 255.0f,
                      GetBValue(value) / 255.0f);
}

auto setting(const wchar_t* key, DWORD default_value) -> DWORD {
  DWORD size = sizeof(DWORD);
  DWORD value = default_value;
  RegGetValueW(HKEY_CURRENT_USER,
               L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", key,
               RRF_RT_REG_DWORD, nullptr, &value, &size);
  return value;
}

auto luminance(D2D1_COLOR_F value) -> double {
  auto linear = [](float channel) -> double {
    return channel <= 0.04045f ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * linear(value.r) + 0.7152 * linear(value.g) + 0.0722 * linear(value.b);
}

auto contrast(D2D1_COLOR_F first, D2D1_COLOR_F second) -> double {
  double first_luminance = luminance(first);
  double second_luminance = luminance(second);
  return (std::max(first_luminance, second_luminance) + 0.05) /
         (std::min(first_luminance, second_luminance) + 0.05);
}

auto accent(Palette& colors, const winrt::Windows::UI::ViewManagement::UISettings& settings)
    -> void {
  using winrt::Windows::UI::ViewManagement::UIColorType;
  constexpr std::array types{UIColorType::AccentDark3,  UIColorType::AccentDark2,
                             UIColorType::AccentDark1,  UIColorType::Accent,
                             UIColorType::AccentLight1, UIColorType::AccentLight2,
                             UIColorType::AccentLight3};
  std::array<D2D1_COLOR_F, types.size()> shades;
  for (std::size_t index = 0; index < types.size(); ++index) {
    auto value = settings.GetColorValue(types[index]);
    shades[index] = D2D1::ColorF(value.R / 255.0f, value.G / 255.0f, value.B / 255.0f);
  }
  // 明暗模式分别从深一阶、浅二阶开始；三个背景共用同一黑/白文字色。
  int base = colors.dark ? 5 : 2;
  colors.accent = shades[base];
  auto black = D2D1::ColorF(0x000000);
  auto white = D2D1::ColorF(0xFFFFFF);
  colors.accent_text =
      contrast(colors.accent, black) >= contrast(colors.accent, white) ? black : white;
  auto readable = [&](int index) {
    // 色阶变化不能牺牲小号按钮文字的对比度；向常态收敛到最近的可读系统色阶。
    while (index != base && contrast(shades[index], colors.accent_text) < 4.5) {
      index += index < base ? 1 : -1;
    }
    return shades[index];
  };
  colors.accent_hover = readable(colors.dark ? base + 1 : base - 1);
  colors.accent_pressed = readable(colors.dark ? base - 1 : base - 2);
}
}  // namespace

auto initialize(core::AppState& state) -> std::expected<void, core::Error> {
  try {
    state.theme->settings = winrt::Windows::UI::ViewManagement::UISettings();
    return refresh(state);
  } catch (const winrt::hresult_error& error) {
    return std::unexpected(core::with_detail(core::error("error.theme.initialize_failed"),
                                             winrt::to_string(error.message())));
  }
}

auto watch(core::AppState& state, HWND target, UINT message) -> std::expected<void, core::Error> {
  auto notification = std::make_shared<Notification>();
  notification->target = target;
  notification->message = message;
  try {
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

auto shutdown(core::AppState& state) -> void {
  auto& theme = *state.theme;
  if (theme.notification) {
    std::lock_guard lock(theme.notification->mutex);
    theme.notification->target = nullptr;
  }
  theme.changed.revoke();
  theme.notification.reset();
}

auto refresh(core::AppState& state) -> std::expected<void, core::Error> {
  try {
    auto mode = state.app->settings.theme;
    bool dark = mode == "dark" || (mode == "system" && setting(L"AppsUseLightTheme", 1) == 0);
    HIGHCONTRASTW contrast{sizeof(HIGHCONTRASTW)};
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
    bool high_contrast = (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
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
      accent(result, state.theme->settings);
    }
    state.theme->colors = result;
    return {};
  } catch (const winrt::hresult_error& error) {
    return std::unexpected(core::with_detail(core::error("error.theme.read_failed"),
                                             winrt::to_string(error.message())));
  }
}

auto resolve(const core::AppState& state) -> const Palette& { return state.theme->colors; }
}  // namespace ui::theme

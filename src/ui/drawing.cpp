#include "ui/drawing.hpp"

#include "vendor/std.hpp"

#include "vendor/windows/dwrite.hpp"

#include "core/i18n/i18n.hpp"
#include "core/state/app_state.hpp"
#include "ui/shared_render_resources/state.hpp"

namespace ui::drawing {

auto fill(ID2D1RenderTarget* target, D2D1_RECT_F rect, D2D1_COLOR_F color, float radius) -> void {
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
  if (SUCCEEDED(target->CreateSolidColorBrush(color, brush.GetAddressOf()))) {
    target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
  }
}

auto outline(ID2D1RenderTarget* target, D2D1_RECT_F rect, D2D1_COLOR_F color, float radius,
             float width) -> void {
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
  if (SUCCEEDED(target->CreateSolidColorBrush(color, brush.GetAddressOf()))) {
    target->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get(), width);
  }
}

auto line(ID2D1RenderTarget* target, D2D1_POINT_2F first, D2D1_POINT_2F second, D2D1_COLOR_F color,
          float width) -> void {
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
  if (SUCCEEDED(target->CreateSolidColorBrush(color, brush.GetAddressOf()))) {
    target->DrawLine(first, second, brush.Get(), width);
  }
}

auto text(core::AppState& state, ID2D1RenderTarget* target, std::wstring_view value,
          D2D1_RECT_F rect, float size, D2D1_COLOR_F color, bool centered,
          const wchar_t* font_family) -> void {
  Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
  auto hr = state.shared_render_resources->write->CreateTextFormat(
      font_family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
      DWRITE_FONT_STRETCH_NORMAL, size,
      core::i18n::current(state) == core::i18n::Language::zh_cn ? L"zh-CN" : L"en-US",
      format.GetAddressOf());
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
  if (FAILED(hr) || FAILED(target->CreateSolidColorBrush(color, brush.GetAddressOf()))) {
    return;
  }
  format->SetTextAlignment(centered ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
  format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
  target->DrawTextW(value.data(), static_cast<UINT32>(value.size()), format.Get(), rect,
                    brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

}  // namespace ui::drawing

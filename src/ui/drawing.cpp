#include "ui/drawing.hpp"

#include "vendor/std.hpp"

#include "vendor/windows/dwrite.hpp"

#include "core/i18n/i18n.hpp"
#include "core/state/app_state.hpp"
#include "ui/shared_render_resources/state.hpp"

namespace ui::drawing {

// 填充纯色圆角矩形：创建纯色画刷 → 调用 FillRoundedRectangle
auto fill(ID2D1RenderTarget* target, D2D1_RECT_F rect, D2D1_COLOR_F color, float radius) -> void {
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
  // 创建指定颜色的 Direct2D 纯色画刷并填充
  if (SUCCEEDED(target->CreateSolidColorBrush(color, brush.GetAddressOf()))) {
    target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
  }
}

// 绘制纯色圆角矩形边框：创建纯色画刷 → 调用 DrawRoundedRectangle
auto outline(ID2D1RenderTarget* target, D2D1_RECT_F rect, D2D1_COLOR_F color, float radius,
             float width) -> void {
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
  // 创建指定颜色的纯色画刷并描边
  if (SUCCEEDED(target->CreateSolidColorBrush(color, brush.GetAddressOf()))) {
    target->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get(), width);
  }
}

// 绘制两点之间的纯色直线段
auto line(ID2D1RenderTarget* target, D2D1_POINT_2F first, D2D1_POINT_2F second, D2D1_COLOR_F color,
          float width) -> void {
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
  // 创建画刷并绘制线段
  if (SUCCEEDED(target->CreateSolidColorBrush(color, brush.GetAddressOf()))) {
    target->DrawLine(first, second, brush.Get(), width);
  }
}

// 渲染格式化文本：构建 DirectWrite 文本格式 → 配置对齐与自动换行 → 裁剪绘制文本
auto text(core::AppState& state, ID2D1RenderTarget* target, std::wstring_view value,
          D2D1_RECT_F rect, float size, D2D1_COLOR_F color, Alignment alignment,
          const wchar_t* font_family) -> void {
  Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
  // 根据当前系统/用户语言及字体尺寸创建 DirectWrite 排版格式
  auto hr = state.shared_render_resources->write->CreateTextFormat(
      font_family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
      DWRITE_FONT_STRETCH_NORMAL, size,
      core::i18n::current(state) == core::i18n::Language::zh_cn ? L"zh-CN" : L"en-US",
      format.GetAddressOf());
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
  if (FAILED(hr) || FAILED(target->CreateSolidColorBrush(color, brush.GetAddressOf()))) {
    return;
  }
  // 配置水平对齐、垂直居中与自动折行属性
  auto dwrite_alignment = DWRITE_TEXT_ALIGNMENT_LEADING;
  if (alignment == Alignment::center) {
    dwrite_alignment = DWRITE_TEXT_ALIGNMENT_CENTER;
  } else if (alignment == Alignment::trailing) {
    dwrite_alignment = DWRITE_TEXT_ALIGNMENT_TRAILING;
  }
  format->SetTextAlignment(dwrite_alignment);
  format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
  // 在指定包围盒内执行裁剪绘制
  target->DrawTextW(value.data(), static_cast<UINT32>(value.size()), format.Get(), rect,
                    brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

}  // namespace ui::drawing

#pragma once

#include "vendor/std.hpp"

#include "vendor/windows/d2d1_1.hpp"

namespace core {
struct AppState;
}

namespace ui::drawing {

enum class Alignment {
  leading,
  center,
  trailing,
};

auto text(core::AppState& state, ID2D1RenderTarget* target, std::wstring_view value,
          D2D1_RECT_F rect, float size, D2D1_COLOR_F color,
          Alignment alignment = Alignment::leading,
          const wchar_t* font_family = L"Microsoft YaHei UI") -> void;
auto fill(ID2D1RenderTarget* target, D2D1_RECT_F rect, D2D1_COLOR_F color, float radius = 0)
    -> void;

// 描边以矩形边缘为中心；绘制到独立 Surface 时须留出半个线宽，避免被裁切。
auto outline(ID2D1RenderTarget* target, D2D1_RECT_F rect, D2D1_COLOR_F color, float radius,
             float width = 1) -> void;
auto line(ID2D1RenderTarget* target, D2D1_POINT_2F first, D2D1_POINT_2F second, D2D1_COLOR_F color,
          float width = 1) -> void;

}  // namespace ui::drawing

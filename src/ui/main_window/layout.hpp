#pragma once
#include "vendor/std.hpp"

#include "vendor/windows.hpp"
#include "vendor/windows/d2d1_1.hpp"

namespace core {
struct AppState;
}

namespace ui::main_window::layout {
// 系统标题栏下方的内容页头保持固定，正文从此处裁剪，单位 DIP。
inline constexpr float header_bottom = 96;
inline constexpr float footer_height = 32;

// 内容画布坐标，单位 DIP；标题栏和内容页头不随正文滚动。
struct Layout {
  float content_height{};
  float body_bottom{};
  float divider_y{};
  D2D1_RECT_F title{};
  D2D1_RECT_F title_separator{};
  D2D1_RECT_F product_name{};
  D2D1_RECT_F settings{};
  D2D1_RECT_F card{};
  D2D1_RECT_F heading{};
  D2D1_RECT_F description{};
  D2D1_RECT_F metrics{};
  D2D1_RECT_F elapsed{};
  D2D1_RECT_F action_description{};
  D2D1_RECT_F primary{};
  D2D1_RECT_F secondary{};
  D2D1_RECT_F details{};
  D2D1_RECT_F save{};
  D2D1_RECT_F diagnostic{};
  D2D1_RECT_F theme{};
  D2D1_RECT_F restore{};
  D2D1_RECT_F language{};
  D2D1_RECT_F summary{};
  D2D1_RECT_F footer{};
  // 覆盖提示条，不参与排版：提示出现或消失不改变其它矩形与滚动范围。
  D2D1_RECT_F notice{};
};

// 只计算 DIP 几何并限制滚动范围，不创建、移动或读取子 HWND。
auto update(core::AppState& state) -> void;
}  // namespace ui::main_window::layout

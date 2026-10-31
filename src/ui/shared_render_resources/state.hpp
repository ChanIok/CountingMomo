#pragma once

#include "vendor/std.hpp"

#include "vendor/windows/d2d1_1.hpp"
#include "vendor/windows/d3d11.hpp"
#include "vendor/windows/dcomp.hpp"
#include "vendor/windows/dwrite.hpp"
#include "vendor/windows/wrl/client.hpp"

namespace ui::shared_render_resources {
struct State {
  Microsoft::WRL::ComPtr<ID3D11Device> d3d;
  Microsoft::WRL::ComPtr<ID2D1Factory1> factory;
  Microsoft::WRL::ComPtr<ID2D1Device> d2d;
  Microsoft::WRL::ComPtr<IDWriteFactory> write;
  Microsoft::WRL::ComPtr<IDCompositionDesktopDevice> composition;
};
}  // namespace ui::shared_render_resources

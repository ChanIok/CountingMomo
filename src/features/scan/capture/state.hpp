#pragma once

#include "vendor/std.hpp"

#include "vendor/windows.hpp"
#include "vendor/windows/d3d11.hpp"
#include "vendor/windows/winrt/windows_foundation.hpp"
#include "vendor/windows/winrt/windows_graphics_capture.hpp"
#include "vendor/windows/winrt/windows_graphics_directx_direct3d11.hpp"

#include "features/scan/capture/types.hpp"

namespace features::scan::capture {

// GPU 帧比对子系统的 D3D11 资源与运行状态
struct ComparisonState {
  winrt::com_ptr<ID3D11Texture2D> reference;
  winrt::com_ptr<ID3D11ShaderResourceView> reference_view;
  winrt::com_ptr<ID3D11ComputeShader> shader;
  winrt::com_ptr<ID3D11Buffer> constants;
  winrt::com_ptr<ID3D11Buffer> changed;
  winrt::com_ptr<ID3D11UnorderedAccessView> changed_view;
  winrt::com_ptr<ID3D11Buffer> changed_readback;
  winrt::com_ptr<ID3D11Query> completed;
  bool reference_valid{};
  bool awaiting_change{};
  std::int64_t stable_since_source{};
};

// 画面捕获会话的整体运行状态与底层句柄
struct State {
  HWND hwnd{};
  Settings settings;
  std::int64_t last_source_time{};
  bool awaiting_target_size{};
  std::mutex signal_mutex;
  winrt::handle frame_ready;
  std::optional<winrt::event_token> frame_token;
  winrt::com_ptr<ID3D11Device> device;
  winrt::com_ptr<ID3D11DeviceContext> context;
  winrt::com_ptr<ID3D11Texture2D> staging;
  ComparisonState comparison;
  winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice runtime_device{nullptr};
  winrt::Windows::Graphics::SizeInt32 pool_size{};
  winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
  winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool{nullptr};
  winrt::Windows::Graphics::Capture::GraphicsCaptureSession session{nullptr};
};

}  // namespace features::scan::capture

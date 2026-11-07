#pragma once

#include "vendor/std.hpp"

#include "vendor/windows.hpp"
#include "vendor/windows/d3d11.hpp"
#include "vendor/windows/winrt/windows_foundation.hpp"
#include "vendor/windows/winrt/windows_graphics_capture.hpp"
#include "vendor/windows/winrt/windows_graphics_directx_direct3d11.hpp"

#include "features/capture/types.hpp"

namespace features::capture {

struct ComparisonState {
  // WGC 帧只负责提供源纹理；后续处理使用自有工作纹理，帧保持到本轮 GPU 命令收尾。
  winrt::com_ptr<ID3D11Texture2D> processing;
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

// 捕获资源仅由扫描线程操作；signal_mutex 只保护跨线程唤醒时的通知句柄寿命。
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

}  // namespace features::capture

#include "features/capture/comparison.hpp"

#include "vendor/std.hpp"

#include "vendor/windows/d3dcompiler.hpp"

#include "core/message/message.hpp"
#include "core/state/app_state.hpp"
#include "features/capture/state.hpp"

namespace features::capture::comparison {

namespace {

constexpr std::string_view shader_source = R"(
Texture2D<float4> current : register(t0);
Texture2D<float4> previous : register(t1);
RWStructuredBuffer<uint> changed : register(u0);

cbuffer Comparison : register(b0) {
    uint2 source_origin;
    uint2 comparison_origin;
    uint2 extent;
    uint2 padding;
};

groupshared uint differences[256];

[numthreads(16, 16, 1)]
void main(uint3 position : SV_DispatchThreadID, uint index : SV_GroupIndex) {
    uint different = 0;
    if (all(position.xy < extent)) {
        uint2 pixel = comparison_origin + position.xy;
        // 直接 Load UNORM 颜色，不采样、不插值；Alpha 不属于交付的 BGR 画面。
        different = any(current.Load(int3(source_origin + pixel, 0)).rgb !=
                        previous.Load(int3(pixel, 0)).rgb) ? 1 : 0;
    }
    differences[index] = different;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 128; stride > 0; stride >>= 1) {
        if (index < stride) {
            differences[index] |= differences[index + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    // 每组至多一次全局原子写，避免变化画面中每个像素争用同一个标记。
    if (index == 0 && differences[0] != 0) {
        InterlockedOr(changed[0], 1);
    }
}
)";

struct Constants {
  std::array<std::uint32_t, 2> source_origin;
  std::array<std::uint32_t, 2> comparison_origin;
  std::array<std::uint32_t, 2> extent;
  std::array<std::uint32_t, 2> padding{};
};

static_assert(sizeof(Constants) == 32);

auto gpu_error(const winrt::hresult_error& error) -> core::Error {
  return core::with_detail(core::error("error.capture.gpu_failed"),
                           winrt::to_string(error.message()));
}

}  // namespace

auto initialize(core::AppState& state) -> std::expected<void, core::Error> {
  auto& capture = *state.capture;
  auto& gpu = capture.comparison;
  try {
    UINT support{};
    winrt::check_hresult(capture.device->CheckFormatSupport(DXGI_FORMAT_B8G8R8A8_UNORM, &support));
    if (!(support & D3D11_FORMAT_SUPPORT_SHADER_LOAD)) {
      return std::unexpected(core::error("error.capture.shader_input_unsupported"));
    }
    winrt::com_ptr<ID3DBlob> bytecode;
    winrt::com_ptr<ID3DBlob> errors;
    auto compiled =
        D3DCompile(shader_source.data(), shader_source.size(), nullptr, nullptr, nullptr, "main",
                   "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, bytecode.put(), errors.put());
    if (FAILED(compiled)) {
      auto failure = core::error("error.capture.shader_compile_failed");
      if (errors) {
        failure.detail.assign(static_cast<const char*>(errors->GetBufferPointer()),
                              errors->GetBufferSize());
      }
      return std::unexpected(std::move(failure));
    }
    winrt::check_hresult(capture.device->CreateComputeShader(
        bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, gpu.shader.put()));

    D3D11_TEXTURE2D_DESC image{};
    image.Width = capture.settings.width;
    image.Height = capture.settings.height;
    image.MipLevels = 1;
    image.ArraySize = 1;
    image.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    image.SampleDesc.Count = 1;
    image.Usage = D3D11_USAGE_DEFAULT;
    image.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    winrt::check_hresult(capture.device->CreateTexture2D(&image, nullptr, gpu.reference.put()));
    winrt::check_hresult(capture.device->CreateShaderResourceView(gpu.reference.get(), nullptr,
                                                                  gpu.reference_view.put()));
    image.Usage = D3D11_USAGE_STAGING;
    image.BindFlags = 0;
    image.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::check_hresult(capture.device->CreateTexture2D(&image, nullptr, capture.staging.put()));

    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(Constants);
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    winrt::check_hresult(capture.device->CreateBuffer(&buffer, nullptr, gpu.constants.put()));
    buffer.ByteWidth = sizeof(std::uint32_t);
    buffer.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    buffer.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    buffer.StructureByteStride = sizeof(std::uint32_t);
    winrt::check_hresult(capture.device->CreateBuffer(&buffer, nullptr, gpu.changed.put()));
    D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
    view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    view.Buffer.NumElements = 1;
    winrt::check_hresult(capture.device->CreateUnorderedAccessView(gpu.changed.get(), &view,
                                                                   gpu.changed_view.put()));
    buffer.Usage = D3D11_USAGE_STAGING;
    buffer.BindFlags = 0;
    buffer.MiscFlags = 0;
    buffer.StructureByteStride = 0;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::check_hresult(
        capture.device->CreateBuffer(&buffer, nullptr, gpu.changed_readback.put()));
    D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
    winrt::check_hresult(capture.device->CreateQuery(&query, gpu.completed.put()));
    return {};
  } catch (const winrt::hresult_error& error) {
    return std::unexpected(gpu_error(error));
  }
}

auto finish(core::AppState& state, std::chrono::steady_clock::time_point deadline,
            const std::function<std::expected<void, core::Error>()>& check)
    -> std::expected<bool, core::Error> {
  auto& capture = *state.capture;
  std::optional<core::Error> interrupted;
  bool timed_out = false;
  try {
    capture.context->End(capture.comparison.completed.get());
    capture.context->Flush();
    while (true) {
      if (!interrupted) {
        if (auto checked = check(); !checked) {
          interrupted = checked.error();
        } else if (std::chrono::steady_clock::now() >= deadline) {
          // 超时不是错误：GPU 访问仍须收尾，完成后由调用方按超时处理。
          timed_out = true;
        }
      }
      BOOL completed{};
      auto status = capture.context->GetData(capture.comparison.completed.get(), &completed,
                                             sizeof(completed), D3D11_ASYNC_GETDATA_DONOTFLUSH);
      winrt::check_hresult(status);
      if (status == S_OK && completed) {
        if (interrupted) {
          return std::unexpected(std::move(*interrupted));
        }
        return !timed_out;
      }
      // 停止可以唤醒检查；已提交的 GPU 访问必须完成后才能归还 WGC 帧。
      if (WaitForSingleObject(capture.frame_ready.get(), 1) == WAIT_FAILED && !interrupted) {
        auto code = GetLastError();
        interrupted = core::with_detail(core::error("error.capture.gpu_failed"),
                                        std::format("Win32={}", code));
      }
    }
  } catch (const winrt::hresult_error& error) {
    if (interrupted) {
      interrupted->related.push_back(gpu_error(error));
      return std::unexpected(std::move(*interrupted));
    }
    return std::unexpected(gpu_error(error));
  }
}

auto update(core::AppState& state, ID3D11Texture2D* source, const RECT& client,
            std::chrono::steady_clock::time_point deadline,
            const std::function<std::expected<void, core::Error>()>& check)
    -> std::expected<std::optional<bool>, core::Error> {
  auto& capture = *state.capture;
  auto& gpu = capture.comparison;
  try {
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    if (!(description.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
        description.Format != DXGI_FORMAT_B8G8R8A8_UNORM || description.ArraySize != 1 ||
        description.SampleDesc.Count != 1) {
      return std::unexpected(core::error("error.capture.shader_input_unsupported"));
    }
    bool different = !gpu.reference_valid;
    if (gpu.reference_valid) {
      D3D11_SHADER_RESOURCE_VIEW_DESC view{};
      view.Format = description.Format;
      view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
      view.Texture2D.MipLevels = 1;
      winrt::com_ptr<ID3D11ShaderResourceView> current;
      winrt::check_hresult(capture.device->CreateShaderResourceView(source, &view, current.put()));
      const auto& area = capture.settings.comparison_rect;
      Constants constants{
          .source_origin = {static_cast<UINT>(client.left), static_cast<UINT>(client.top)},
          .comparison_origin = {static_cast<UINT>(area[0]), static_cast<UINT>(area[1])},
          .extent = {static_cast<UINT>(area[2] - area[0]), static_cast<UINT>(area[3] - area[1])},
      };
      capture.context->UpdateSubresource(gpu.constants.get(), 0, nullptr, &constants, 0, 0);
      constexpr UINT clear[4]{};
      capture.context->ClearUnorderedAccessViewUint(gpu.changed_view.get(), clear);
      ID3D11ShaderResourceView* inputs[]{current.get(), gpu.reference_view.get()};
      ID3D11UnorderedAccessView* outputs[]{gpu.changed_view.get()};
      ID3D11Buffer* buffers[]{gpu.constants.get()};
      capture.context->CSSetShader(gpu.shader.get(), nullptr, 0);
      capture.context->CSSetShaderResources(0, 2, inputs);
      capture.context->CSSetUnorderedAccessViews(0, 1, outputs, nullptr);
      capture.context->CSSetConstantBuffers(0, 1, buffers);
      capture.context->Dispatch((constants.extent[0] + 15) / 16, (constants.extent[1] + 15) / 16,
                                1);
      // 比较结束后解除输入／输出绑定，再做读回或覆盖参考纹理。
      ID3D11ShaderResourceView* no_inputs[2]{};
      ID3D11UnorderedAccessView* no_outputs[1]{};
      ID3D11Buffer* no_buffers[1]{};
      capture.context->CSSetShaderResources(0, 2, no_inputs);
      capture.context->CSSetUnorderedAccessViews(0, 1, no_outputs, nullptr);
      capture.context->CSSetConstantBuffers(0, 1, no_buffers);
      capture.context->CSSetShader(nullptr, nullptr, 0);
      capture.context->CopyResource(gpu.changed_readback.get(), gpu.changed.get());
      auto completed = finish(state, deadline, check);
      if (!completed) {
        return std::unexpected(completed.error());
      }
      if (!*completed) {
        return std::optional<bool>{};
      }
      D3D11_MAPPED_SUBRESOURCE mapped{};
      // finish() 已等待 GPU 查询完成；阻塞式读回避免把瞬时 GPU 拥堵误报为致命错误。
      winrt::check_hresult(
          capture.context->Map(gpu.changed_readback.get(), 0, D3D11_MAP_READ, 0, &mapped));
      different = *static_cast<const std::uint32_t*>(mapped.pData) != 0;
      capture.context->Unmap(gpu.changed_readback.get(), 0);
    }
    if (different) {
      D3D11_BOX box{static_cast<UINT>(client.left),  static_cast<UINT>(client.top),    0,
                    static_cast<UINT>(client.right), static_cast<UINT>(client.bottom), 1};
      capture.context->CopySubresourceRegion(gpu.reference.get(), 0, 0, 0, 0, source, 0, &box);
      auto completed = finish(state, deadline, check);
      if (!completed) {
        return std::unexpected(completed.error());
      }
      if (!*completed) {
        return std::optional<bool>{};
      }
      gpu.reference_valid = true;
    }
    return std::optional<bool>{different};
  } catch (const winrt::hresult_error& error) {
    return std::unexpected(gpu_error(error));
  }
}

}  // namespace features::capture::comparison

#include "features/scan/capture/comparison.hpp"

#include "vendor/std.hpp"

#include "vendor/windows/d3dcompiler.hpp"

#include "core/message/message.hpp"
#include "core/state/app_state.hpp"
#include "features/scan/capture/state.hpp"
#include "features/scan/state.hpp"

namespace features::scan::capture::comparison {

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

// 将 WinRT HRESULT 异常包装为项目标准 GPU 错误对象
auto gpu_error(const winrt::hresult_error& error) -> core::Error {
  return core::with_detail(core::error("error.capture.gpu_failed"),
                           winrt::to_string(error.message()));
}

}  // namespace

// 初始化 GPU 比对子系统：编译计算着色器 → 创建参考纹理、常量缓冲区与查询事件
auto initialize(core::AppState& state) -> std::expected<void, core::Error> {
  auto& capture = state.scan->capture;
  auto& gpu = capture.comparison;
  try {
    // 检查设备是否支持 B8G8R8A8_UNORM 着色器加载
    UINT support{};
    winrt::check_hresult(capture.device->CheckFormatSupport(DXGI_FORMAT_B8G8R8A8_UNORM, &support));
    if (!(support & D3D11_FORMAT_SUPPORT_SHADER_LOAD)) {
      return std::unexpected(core::error("error.capture.shader_input_unsupported"));
    }
    // 编译 HLSL 计算着色器代码
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
    // 创建计算着色器对象
    winrt::check_hresult(capture.device->CreateComputeShader(
        bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, gpu.shader.put()));

    // 创建参考纹理及其着色器资源视图 (SRV)
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
    // 创建用于向 CPU 读回完整画面的暂存纹理 (Staging Texture)
    image.Usage = D3D11_USAGE_STAGING;
    image.BindFlags = 0;
    image.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::check_hresult(capture.device->CreateTexture2D(&image, nullptr, capture.staging.put()));

    // 创建比对区域参数的常量缓冲区
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(Constants);
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    winrt::check_hresult(capture.device->CreateBuffer(&buffer, nullptr, gpu.constants.put()));
    // 创建记录是否有像素变化的输出缓冲区及其无序访问视图 (UAV)
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
    // 创建用于 CPU 读回变化标记的暂存缓冲区
    buffer.Usage = D3D11_USAGE_STAGING;
    buffer.BindFlags = 0;
    buffer.MiscFlags = 0;
    buffer.StructureByteStride = 0;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::check_hresult(
        capture.device->CreateBuffer(&buffer, nullptr, gpu.changed_readback.put()));
    // 创建同步事件查询对象
    D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
    winrt::check_hresult(capture.device->CreateQuery(&query, gpu.completed.put()));
    return {};
  } catch (const winrt::hresult_error& error) {
    return std::unexpected(gpu_error(error));
  }
}

// 等待已提交的 GPU 比对与拷贝命令执行完成
auto finish(core::AppState& state, std::chrono::steady_clock::time_point deadline,
            const std::function<std::expected<void, core::Error>()>& check)
    -> std::expected<bool, core::Error> {
  auto& capture = state.scan->capture;
  std::optional<core::Error> interrupted;
  bool timed_out = false;
  try {
    // 插入事件查询标记并刷新命令队列至 GPU
    capture.context->End(capture.comparison.completed.get());
    capture.context->Flush();
    // 轮询等待 GPU 执行完成
    while (true) {
      if (!interrupted) {
        // 检查停止请求或外部中断
        if (auto checked = check(); !checked) {
          interrupted = checked.error();
        } else if (std::chrono::steady_clock::now() >= deadline) {
          timed_out = true;
        }
      }
      // 查询 GPU 是否已完成执行
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
      // 阻塞等待唤醒事件或 1ms 超时轮询
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

// 执行帧比对与参考帧更新：调度着色器比对差分 → 读回结果 → 若变化则拷贝当前帧为新参考
auto update(core::AppState& state, ID3D11Texture2D* source, const RECT& client,
            std::chrono::steady_clock::time_point deadline,
            const std::function<std::expected<void, core::Error>()>& check)
    -> std::expected<std::optional<bool>, core::Error> {
  auto& capture = state.scan->capture;
  auto& gpu = capture.comparison;
  try {
    // 校验输入纹理格式与绑定属性
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    if (!(description.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
        description.Format != DXGI_FORMAT_B8G8R8A8_UNORM || description.ArraySize != 1 ||
        description.SampleDesc.Count != 1) {
      return std::unexpected(core::error("error.capture.shader_input_unsupported"));
    }
    // 若此前没有有效参考帧，直接视为有变化
    bool different = !gpu.reference_valid;
    if (gpu.reference_valid) {
      // 为当前源帧创建临时 SRV
      D3D11_SHADER_RESOURCE_VIEW_DESC view{};
      view.Format = description.Format;
      view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
      view.Texture2D.MipLevels = 1;
      winrt::com_ptr<ID3D11ShaderResourceView> current;
      winrt::check_hresult(capture.device->CreateShaderResourceView(source, &view, current.put()));
      // 更新比对矩形常量数据
      const auto& area = capture.settings.comparison_rect;
      Constants constants{
          .source_origin = {static_cast<UINT>(client.left), static_cast<UINT>(client.top)},
          .comparison_origin = {static_cast<UINT>(area[0]), static_cast<UINT>(area[1])},
          .extent = {static_cast<UINT>(area[2] - area[0]), static_cast<UINT>(area[3] - area[1])},
      };
      capture.context->UpdateSubresource(gpu.constants.get(), 0, nullptr, &constants, 0, 0);
      // 清零变化标记缓冲区
      constexpr UINT clear[4]{};
      capture.context->ClearUnorderedAccessViewUint(gpu.changed_view.get(), clear);
      // 绑定着色器、输入纹理与输出缓冲区
      ID3D11ShaderResourceView* inputs[]{current.get(), gpu.reference_view.get()};
      ID3D11UnorderedAccessView* outputs[]{gpu.changed_view.get()};
      ID3D11Buffer* buffers[]{gpu.constants.get()};
      capture.context->CSSetShader(gpu.shader.get(), nullptr, 0);
      capture.context->CSSetShaderResources(0, 2, inputs);
      capture.context->CSSetUnorderedAccessViews(0, 1, outputs, nullptr);
      capture.context->CSSetConstantBuffers(0, 1, buffers);
      // 派发计算着色器线程组（每组 16x16 像素）
      capture.context->Dispatch((constants.extent[0] + 15) / 16, (constants.extent[1] + 15) / 16,
                                1);
      // 解绑管线资源，准备拷贝结果
      ID3D11ShaderResourceView* no_inputs[2]{};
      ID3D11UnorderedAccessView* no_outputs[1]{};
      ID3D11Buffer* no_buffers[1]{};
      capture.context->CSSetShaderResources(0, 2, no_inputs);
      capture.context->CSSetUnorderedAccessViews(0, 1, no_outputs, nullptr);
      capture.context->CSSetConstantBuffers(0, 1, no_buffers);
      capture.context->CSSetShader(nullptr, nullptr, 0);
      // 将 GPU 标记缓冲区拷贝到 CPU 可读的 Staging 缓冲
      capture.context->CopyResource(gpu.changed_readback.get(), gpu.changed.get());
      // 等待 GPU 命令执行完毕
      auto completed = finish(state, deadline, check);
      if (!completed) {
        return std::unexpected(completed.error());
      }
      if (!*completed) {
        return std::optional<bool>{};
      }
      // 映射读取变化标记
      D3D11_MAPPED_SUBRESOURCE mapped{};
      winrt::check_hresult(
          capture.context->Map(gpu.changed_readback.get(), 0, D3D11_MAP_READ, 0, &mapped));
      different = *static_cast<const std::uint32_t*>(mapped.pData) != 0;
      capture.context->Unmap(gpu.changed_readback.get(), 0);
    }
    // 若画面发生变化，将当前源帧的客户区拷贝为新的参考纹理
    if (different) {
      D3D11_BOX box{static_cast<UINT>(client.left),  static_cast<UINT>(client.top),    0,
                    static_cast<UINT>(client.right), static_cast<UINT>(client.bottom), 1};
      capture.context->CopySubresourceRegion(gpu.reference.get(), 0, 0, 0, 0, source, 0, &box);
      // 等待纹理拷贝完成
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

}  // namespace features::scan::capture::comparison

#include "features/scan/capture/capture.hpp"

#include "vendor/std.hpp"

#include "vendor/windows.hpp"
#include "vendor/windows/dwmapi.hpp"
#include "vendor/windows/dxgi.hpp"
#include "vendor/windows/windows_graphics_capture_interop.hpp"
#include "vendor/windows/windows_graphics_directx_direct3d11_interop.hpp"
#include "vendor/windows/winrt/windows_foundation_metadata.hpp"
#include "vendor/windows/winrt/windows_graphics_directx.hpp"

#include "core/message/message.hpp"
#include "core/state/app_state.hpp"
#include "features/scan/capture/comparison.hpp"
#include "features/scan/capture/state.hpp"
#include "features/scan/state.hpp"

namespace features::scan::capture {

namespace {

constexpr int frame_pool_size = 1;

// 执行单个清理操作并捕获异常，确保后续释放流程继续推进
auto attempt(auto&& operation, std::string_view key) -> std::expected<void, core::Error> {
  try {
    operation();
    return {};
  } catch (const winrt::hresult_error& error) {
    return std::unexpected(core::with_detail(core::error(key), winrt::to_string(error.message())));
  } catch (const std::exception& error) {
    return std::unexpected(core::with_detail(core::error(key), error.what()));
  } catch (...) {
    return std::unexpected(core::error(key));
  }
}

// 计算客户区在全窗口捕获帧中的裁剪矩形
auto client_box(HWND hwnd, int frame_width, int frame_height, int width, int height)
    -> std::expected<std::optional<cv::Rect>, core::Error> {
  // 捕获帧尺寸与目标尺寸一致时直接返回全画幅
  if (frame_width == width && frame_height == height) {
    return std::optional{cv::Rect(0, 0, width, height)};
  }
  // 获取客户区左上角在屏幕上的绝对物理坐标
  POINT origin{};
  if (!ClientToScreen(hwnd, &origin)) {
    return std::unexpected(core::error("error.capture.client_origin_failed"));
  }
  std::vector<RECT> bounds;
  RECT window_rect{};
  // 优先读取含阴影的 DWM 扩展窗口外接边界
  if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &window_rect,
                                      sizeof(window_rect)))) {
    bounds.push_back(window_rect);
  }
  // 回退尝试常规 Win32 窗口边界
  if (GetWindowRect(hwnd, &window_rect)) {
    bounds.push_back(window_rect);
  }
  // 匹配与当前帧像素尺寸一致的边界并计算客户区相对位移
  for (auto bounds_rect : bounds) {
    int x = origin.x - bounds_rect.left;
    int y = origin.y - bounds_rect.top;
    if (bounds_rect.right - bounds_rect.left == frame_width &&
        bounds_rect.bottom - bounds_rect.top == frame_height && x >= 0 && y >= 0 &&
        x + width <= frame_width && y + height <= frame_height) {
      return std::optional{cv::Rect(x, y, width, height)};
    }
  }
  return std::optional<cv::Rect>{};
}

// 处理单帧捕获图像：尺寸自适应 → GPU 判稳 → 达到稳定阈值则读回完整 BGR 图像
auto process_frame(core::AppState& state,
                   winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame& frame,
                   std::chrono::steady_clock::time_point deadline,
                   const std::function<std::expected<void, core::Error>()>& check)
    -> std::expected<std::optional<Frame>, core::Error> {
  auto& capture = state.scan->capture;
  const auto& settings = capture.settings;
  auto source = frame.SystemRelativeTime().count();
  auto content = frame.ContentSize();
  if (content.Width <= 0 || content.Height <= 0) {
    return {};
  }
  // 若捕获帧尺寸发生变化，重建帧池以适配新尺寸
  if (capture.awaiting_target_size &&
      (content.Width != capture.pool_size.Width || content.Height != capture.pool_size.Height)) {
    frame.Close();
    frame = nullptr;
    capture.pool.Recreate(
        capture.runtime_device,
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        frame_pool_size, content);
    capture.pool_size = content;
    return {};
  }
  // 获取捕获帧的底层 D3D11 纹理
  auto access =
      frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
  winrt::com_ptr<ID3D11Texture2D> texture;
  winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
  D3D11_TEXTURE2D_DESC description{};
  texture->GetDesc(&description);
  if (content.Width > static_cast<int>(description.Width) ||
      content.Height > static_cast<int>(description.Height)) {
    return {};
  }
  // 计算客户区裁剪矩形
  auto box =
      client_box(capture.hwnd, content.Width, content.Height, settings.width, settings.height);
  if (!box) {
    return std::unexpected(box.error());
  }
  if (!*box) {
    if (!capture.awaiting_target_size) {
      return std::unexpected(core::error("error.capture.client_region_unknown"));
    }
    return {};
  }
  capture.awaiting_target_size = false;
  const auto& area = **box;
  RECT client{area.x, area.y, area.x + area.width, area.y + area.height};
  auto& gpu = capture.comparison;
  // 通过 GPU 计算着色器比对指定区域是否发生像素变化
  auto changed = comparison::update(state, texture.get(), client, deadline, check);
  if (!changed) {
    return std::unexpected(changed.error());
  }
  // 达到 deadline 仍未完成
  if (!*changed) {
    return std::optional<Frame>{};
  }
  // 画面发生变化：重置静止起始时间戳并清除等待标记
  if (**changed) {
    gpu.awaiting_change = false;
    gpu.stable_since_source = source;
  } else if (gpu.awaiting_change) {
    return {};  // 仍在等待发生首个变化
  } else if (gpu.stable_since_source == 0) {
    gpu.stable_since_source = source;
  }
  // 检查静止持续时长是否已达到稳定阈值
  double stable_seconds = static_cast<double>(source - gpu.stable_since_source) / 10'000'000.0;
  if (source == gpu.stable_since_source || stable_seconds < settings.stable_seconds) {
    return {};
  }

  // 拷贝客户区画面至 Staging 纹理准备读回 CPU
  D3D11_BOX source_box{static_cast<UINT>(client.left),  static_cast<UINT>(client.top),    0,
                       static_cast<UINT>(client.right), static_cast<UINT>(client.bottom), 1};
  capture.context->CopySubresourceRegion(capture.staging.get(), 0, 0, 0, 0, texture.get(), 0,
                                         &source_box);
  // 等待 GPU 拷贝操作完成
  auto completed = comparison::finish(state, deadline, check);
  if (!completed) {
    return std::unexpected(completed.error());
  }
  if (!*completed) {
    return std::optional<Frame>{};
  }
  // 映射内存并转换为 OpenCV BGR 格式
  D3D11_MAPPED_SUBRESOURCE mapped{};
  winrt::check_hresult(capture.context->Map(capture.staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
  cv::Mat bgr;
  try {
    cv::Mat bgra(settings.height, settings.width, CV_8UC4, mapped.pData, mapped.RowPitch);
    cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
  } catch (...) {
    capture.context->Unmap(capture.staging.get(), 0);
    throw;
  }
  capture.context->Unmap(capture.staging.get(), 0);
  return Frame{
      .source_time = source,
      .bgr = std::move(bgr),
  };
}

// 从帧池拉取最新帧并执行稳定评估
auto read_frame(core::AppState& state, std::int64_t after_source_time,
                std::chrono::steady_clock::time_point deadline,
                const std::function<std::expected<void, core::Error>()>& check)
    -> std::expected<std::optional<Frame>, core::Error> {
  auto& capture = state.scan->capture;
  winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame{nullptr};
  std::expected<std::optional<Frame>, core::Error> result;
  try {
    // 从帧池尝试获取下一个可用捕获帧
    frame = capture.pool.TryGetNextFrame();
    // 仅处理发生在操作时间戳之后的新帧
    if (frame && frame.SystemRelativeTime().count() >
                     std::max(after_source_time, capture.last_source_time)) {
      capture.last_source_time = frame.SystemRelativeTime().count();
      result = process_frame(state, frame, deadline, check);
    }
  } catch (const winrt::hresult_error& error) {
    result = std::unexpected(core::with_detail(core::error("error.capture.read_failed"),
                                               winrt::to_string(error.message())));
  } catch (const std::exception& error) {
    result =
        std::unexpected(core::with_detail(core::error("error.capture.read_failed"), error.what()));
  } catch (...) {
    result = std::unexpected(core::error("error.capture.read_failed"));
  }
  // 显式关闭帧句柄释放资源
  if (frame) {
    auto closed = attempt([&] { frame.Close(); }, "error.capture.read_failed");
    if (!closed) {
      if (result) {
        result = std::unexpected(closed.error());
      } else {
        result.error().related.push_back(closed.error());
      }
    }
  }
  if (!result) {
    return std::unexpected(result.error());
  }
  return result;
}

}  // namespace

// 获取当前系统高精度 QPC 时间戳（单位为 100ns）
auto source_time() -> std::int64_t {
  LARGE_INTEGER counter{};
  LARGE_INTEGER frequency{};
  // 查询高精度计数器与时钟频率
  QueryPerformanceCounter(&counter);
  QueryPerformanceFrequency(&frequency);
  constexpr std::int64_t ticks_per_second = 10'000'000;
  // 换算为 100ns 刻度，分段处理避免溢出
  return counter.QuadPart / frequency.QuadPart * ticks_per_second +
         static_cast<std::int64_t>(static_cast<long double>(counter.QuadPart % frequency.QuadPart) *
                                   ticks_per_second / frequency.QuadPart);
}

// 启动 WGC 捕获会话：初始化 D3D11 设备 → 创建捕获帧池与会话 → 开启不含光标的帧捕获
auto start(core::AppState& state, HWND hwnd, Settings settings)
    -> std::expected<void, core::Error> {
  auto& capture = state.scan->capture;
  // 防止重复启动
  if (capture.session || capture.pool || capture.item) {
    return std::unexpected(core::error("error.capture.already_started"));
  }
  // 校验捕获与比对设置的尺寸有效性
  const auto& area = settings.comparison_rect;
  if (settings.width <= 0 || settings.height <= 0 || settings.width > 16384 ||
      settings.height > 16384 || area[0] < 0 || area[1] < 0 || area[2] <= area[0] ||
      area[3] <= area[1] || area[2] > settings.width || area[3] > settings.height ||
      !std::isfinite(settings.stable_seconds) || settings.stable_seconds <= 0) {
    return std::unexpected(core::error("error.capture.invalid_settings"));
  }
  core::Error failure;
  try {
    // 检查系统是否支持 Windows Graphics Capture API
    if (!winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported()) {
      return std::unexpected(core::error("error.capture.unsupported"));
    }
    // 检查是否支持禁用鼠标光标捕获
    if (!winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
            winrt::name_of<winrt::Windows::Graphics::Capture::GraphicsCaptureSession>(),
            L"IsCursorCaptureEnabled")) {
      return std::unexpected(core::error("error.capture.cursor_control_unsupported"));
    }
    // 创建帧就绪通知事件及其独立副本
    winrt::handle frame_ready{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    winrt::check_bool(static_cast<bool>(frame_ready));
    winrt::handle callback_signal;
    winrt::check_bool(DuplicateHandle(GetCurrentProcess(), frame_ready.get(), GetCurrentProcess(),
                                      callback_signal.put(), EVENT_MODIFY_STATE, FALSE, 0));
    {
      std::lock_guard lock(capture.signal_mutex);
      capture.frame_ready = std::move(frame_ready);
    }
    capture.hwnd = hwnd;
    capture.settings = settings;
    capture.awaiting_target_size = true;
    // 创建 D3D11 渲染设备与即时上下文
    constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
    winrt::check_hresult(D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1,
        D3D11_SDK_VERSION, capture.device.put(), nullptr, capture.context.put()));
    // 初始化 GPU 比对子系统
    if (auto initialized = comparison::initialize(state); !initialized) {
      failure = initialized.error();
    } else {
      // 获取 WinRT 运行时 Direct3D 设备接口
      auto dxgi = capture.device.as<IDXGIDevice>();
      winrt::com_ptr<IInspectable> inspectable;
      winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
      capture.runtime_device =
          inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
      // 为目标窗口句柄创建 GraphicsCaptureItem
      auto interop =
          winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                                        IGraphicsCaptureItemInterop>();
      winrt::check_hresult(interop->CreateForWindow(
          hwnd, winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
          winrt::put_abi(capture.item)));
      capture.pool_size = capture.item.Size();
      // 创建捕获帧池
      capture.pool =
          winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
              capture.runtime_device,
              winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
              frame_pool_size, capture.pool_size);
      // 注册帧到达事件回调，仅发送信号触发事件通知
      capture.frame_token =
          capture.pool.FrameArrived([signal = std::move(callback_signal)](auto&&, auto&&) noexcept {
            SetEvent(signal.get());
          });
      // 创建并启动捕获会话，关闭光标捕获
      capture.session = capture.pool.CreateCaptureSession(capture.item);
      capture.session.IsCursorCaptureEnabled(false);
      capture.session.StartCapture();
      return {};
    }
  } catch (const winrt::hresult_error& error) {
    failure = core::with_detail(core::error("error.capture.start_failed"),
                                winrt::to_string(error.message()));
  } catch (const std::exception& error) {
    failure = core::with_detail(core::error("error.capture.start_failed"), error.what());
  } catch (...) {
    failure = core::error("error.capture.start_failed");
  }
  // 启动失败时回滚清理已分配的资源
  if (auto stopped = stop(state); !stopped) {
    failure.related.push_back(stopped.error());
  }
  return std::unexpected(std::move(failure));
}

// 阻塞等待画面稳定：轮询接收新帧 → GPU 判定比对区变化 → 达到稳定时长后读回 BGR 图像
auto wait_stable_frame(core::AppState& state, std::int64_t after_source_time,
                       std::chrono::steady_clock::time_point deadline,
                       const std::function<std::expected<void, core::Error>()>& check,
                       bool require_change) -> std::expected<std::optional<Frame>, core::Error> {
  auto& capture = state.scan->capture;
  // 确认会话已启动
  if (!capture.session) {
    return std::unexpected(core::error("error.capture.not_started"));
  }
  capture.comparison.stable_since_source = 0;
  capture.comparison.awaiting_change = require_change;
  // 循环等待画面稳定
  while (true) {
    // 检查停止请求
    if (auto checked = check(); !checked) {
      return std::unexpected(checked.error());
    }
    // 检查目标窗口是否仍然有效
    if (!IsWindow(capture.hwnd)) {
      return std::unexpected(core::error("error.capture.window_closed"));
    }
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return std::optional<Frame>{};
    }
    // 阻塞等待帧到达信号（分片 50ms 超时轮询检测取消）
    auto until_check = std::min(deadline, now + std::chrono::milliseconds(50));
    auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(until_check - now).count();
    auto waited = WaitForSingleObject(capture.frame_ready.get(), static_cast<DWORD>(milliseconds));
    if (waited == WAIT_TIMEOUT) {
      continue;
    }
    if (waited == WAIT_FAILED) {
      auto code = GetLastError();
      return std::unexpected(core::with_detail(core::error("error.capture.read_failed"),
                                               std::format("Win32={}", code)));
    }
    // 被唤醒后优先检查取消状态
    if (auto checked = check(); !checked) {
      return std::unexpected(checked.error());
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return std::optional<Frame>{};
    }
    // 读取并评估新帧
    auto frame = read_frame(state, after_source_time, deadline, check);
    if (!frame) {
      return std::unexpected(frame.error());
    }
    if (!*frame) {
      continue;
    }
    // 成功获得稳定帧，做最后一次取消检查并返回
    if (auto checked = check(); !checked) {
      return std::unexpected(checked.error());
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return std::optional<Frame>{};
    }
    return std::move(**frame);
  }
}

// 跨线程唤醒帧等待流程（通常由取消/停止操作触发）
auto wake(core::AppState& state) -> void {
  auto& capture = state.scan->capture;
  std::lock_guard lock(capture.signal_mutex);
  // 激活事件句柄唤醒等待线程
  if (capture.frame_ready) {
    SetEvent(capture.frame_ready.get());
  }
}

// 停止捕获会话并释放全部 WGC 与 D3D 资源
auto stop(core::AppState& state) -> std::expected<void, core::Error> {
  auto& capture = state.scan->capture;
  std::expected<void, core::Error> result;
  // 依次尝试关闭各项资源并收集错误
  auto close = [&](auto&& operation) {
    auto closed = attempt(operation, "error.capture.stop_failed");
    if (!closed) {
      if (result) {
        result = std::unexpected(closed.error());
      } else {
        result.error().related.push_back(closed.error());
      }
    }
  };
  // 关闭捕获会话
  if (capture.session) {
    close([&] { capture.session.Close(); });
  }
  // 退订事件并关闭帧池
  if (capture.pool) {
    if (capture.frame_token) {
      close([&] { capture.pool.FrameArrived(*capture.frame_token); });
    }
    close([&] { capture.pool.Close(); });
  }
  // 清理全部 COM 指针与内部状态
  capture.frame_token.reset();
  capture.session = nullptr;
  capture.pool = nullptr;
  capture.item = nullptr;
  capture.comparison = {};
  capture.staging = nullptr;
  capture.context = nullptr;
  capture.runtime_device = nullptr;
  capture.device = nullptr;
  capture.pool_size = {};
  capture.hwnd = nullptr;
  capture.settings = {};
  capture.last_source_time = 0;
  capture.awaiting_target_size = false;
  // 关闭通知事件句柄
  {
    std::lock_guard lock(capture.signal_mutex);
    capture.frame_ready.close();
  }
  return result;
}

}  // namespace features::scan::capture

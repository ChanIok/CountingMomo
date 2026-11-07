#include "features/capture/capture.hpp"

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
#include "features/capture/comparison.hpp"
#include "features/capture/state.hpp"

namespace features::capture {

namespace wgc = winrt::Windows::Graphics::Capture;
namespace direct = winrt::Windows::Graphics::DirectX;

namespace {

constexpr int frame_pool_size = 1;
using Clock = std::chrono::steady_clock;

// 清理动作独立执行，前一步失败也不跳过后续资源。
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

// 计算客户区在捕获帧中的裁剪矩形：匹配全屏尺寸 → 获取客户区原点与 DWM 扩展边框 → 换算相对偏移
auto client_box(HWND hwnd, int frame_width, int frame_height, int width, int height)
    -> std::expected<std::optional<cv::Rect>, core::Error> {
  // 捕获帧尺寸与目标尺寸完全吻合时直接返回全画幅
  if (frame_width == width && frame_height == height) {
    return std::optional{cv::Rect(0, 0, width, height)};
  }
  // 获取窗口客户区左上角在屏幕上的绝对物理坐标
  POINT origin{};
  if (!ClientToScreen(hwnd, &origin)) {
    return std::unexpected(core::error("error.capture.client_origin_failed"));
  }
  std::vector<RECT> bounds;
  RECT window_rect{};
  // 优先尝试读取包含阴影的 DWM 扩展窗口外接边界
  if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &window_rect,
                                      sizeof(window_rect)))) {
    bounds.push_back(window_rect);
  }
  // 回退尝试常规 Win32 窗口矩形
  if (GetWindowRect(hwnd, &window_rect)) {
    bounds.push_back(window_rect);
  }
  // 在候选窗口边界中匹配与当前帧像素尺寸一致的边界并计算客户区相对位移
  for (auto bounds_rect : bounds) {
    int x = origin.x - bounds_rect.left;
    int y = origin.y - bounds_rect.top;
    if (bounds_rect.right - bounds_rect.left == frame_width &&
        bounds_rect.bottom - bounds_rect.top == frame_height && x >= 0 && y >= 0 &&
        x + width <= frame_width && y + height <= frame_height) {
      return std::optional{cv::Rect(x, y, width, height)};
    }
  }
  // 尚未捕获到匹配目标几何的帧时返回空可选值，由调用者重试
  return std::optional<cv::Rect>{};
}

// WGC 帧在 GPU 判稳和稳定读回完成前保持有效；只有稳定候选才读回完整客户区。
auto process_frame(core::AppState& state, wgc::Direct3D11CaptureFrame& frame,
                   Clock::time_point deadline,
                   const std::function<std::expected<void, core::Error>()>& check)
    -> std::expected<std::optional<Frame>, core::Error> {
  auto& capture = *state.capture;
  const auto& settings = capture.settings;
  auto source = frame.SystemRelativeTime().count();
  auto content = frame.ContentSize();
  if (content.Width <= 0 || content.Height <= 0) {
    return {};
  }
  if (capture.awaiting_target_size &&
      (content.Width != capture.pool_size.Width || content.Height != capture.pool_size.Height)) {
    frame.Close();
    frame = nullptr;
    capture.pool.Recreate(capture.runtime_device,
                          direct::DirectXPixelFormat::B8G8R8A8UIntNormalized, frame_pool_size,
                          content);
    capture.pool_size = content;
    return {};
  }
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
  auto changed = comparison::update(state, texture.get(), client, deadline, check);
  if (!changed) {
    return std::unexpected(changed.error());
  }
  // 未在 deadline 内得出比较结果，交回等待流程判定超时。
  if (!*changed) {
    return std::optional<Frame>{};
  }
  if (**changed) {
    gpu.awaiting_change = false;
    gpu.stable_since_source = source;
  } else if (gpu.awaiting_change) {
    return {};
  } else if (gpu.stable_since_source == 0) {
    gpu.stable_since_source = source;
  }
  double stable_seconds = static_cast<double>(source - gpu.stable_since_source) / 10'000'000.0;
  if (source == gpu.stable_since_source || stable_seconds < settings.stable_seconds) {
    return {};
  }

  // 读回当前 WGC 帧，而非参考纹理：比较域之外也应交付当前完整画面。
  D3D11_BOX source_box{static_cast<UINT>(client.left),  static_cast<UINT>(client.top),    0,
                       static_cast<UINT>(client.right), static_cast<UINT>(client.bottom), 1};
  capture.context->CopySubresourceRegion(capture.staging.get(), 0, 0, 0, 0, texture.get(), 0,
                                         &source_box);
  auto completed = comparison::finish(state, deadline, check);
  if (!completed) {
    return std::unexpected(completed.error());
  }
  if (!*completed) {
    return std::optional<Frame>{};
  }
  D3D11_MAPPED_SUBRESOURCE mapped{};
  // finish() 已等待 GPU 查询完成；阻塞式读回避免把瞬时 GPU 拥堵误报为致命错误。
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

// 一次通知只尝试取一帧；空池、旧源帧和初始化期间的尺寸收敛均交回等待流程。
auto read_frame(core::AppState& state, std::int64_t after_source_time, Clock::time_point deadline,
                const std::function<std::expected<void, core::Error>()>& check)
    -> std::expected<std::optional<Frame>, core::Error> {
  auto& capture = *state.capture;
  wgc::Direct3D11CaptureFrame frame{nullptr};
  std::expected<std::optional<Frame>, core::Error> result;
  try {
    frame = capture.pool.TryGetNextFrame();
    // 在 GPU 读回前排除操作前的帧，避免为不可能被采用的画面做转换。
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

auto source_time() -> std::int64_t {
  LARGE_INTEGER counter{};
  LARGE_INTEGER frequency{};
  // 项目要求 Windows 10；此平台对有效参数保证 QPC 和频率查询成功。
  QueryPerformanceCounter(&counter);
  QueryPerformanceFrequency(&frequency);
  constexpr std::int64_t ticks_per_second = 10'000'000;
  // 整秒和余数分开换算，避免长时间开机后整段计数相乘溢出或丢失精度。
  return counter.QuadPart / frequency.QuadPart * ticks_per_second +
         static_cast<std::int64_t>(static_cast<long double>(counter.QuadPart % frequency.QuadPart) *
                                   ticks_per_second / frequency.QuadPart);
}

auto start(core::AppState& state, HWND hwnd, Settings settings)
    -> std::expected<void, core::Error> {
  auto& capture = *state.capture;
  if (capture.session || capture.pool || capture.item) {
    return std::unexpected(core::error("error.capture.already_started"));
  }
  const auto& area = settings.comparison_rect;
  if (settings.width <= 0 || settings.height <= 0 || settings.width > 16384 ||
      settings.height > 16384 || area[0] < 0 || area[1] < 0 || area[2] <= area[0] ||
      area[3] <= area[1] || area[2] > settings.width || area[3] > settings.height ||
      !std::isfinite(settings.stable_seconds) || settings.stable_seconds <= 0) {
    return std::unexpected(core::error("error.capture.invalid_settings"));
  }
  core::Error failure;
  try {
    if (!wgc::GraphicsCaptureSession::IsSupported()) {
      return std::unexpected(core::error("error.capture.unsupported"));
    }
    // 光标像素会干扰匹配与判稳，缺少排除能力时不能继续扫描。
    if (!winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
            winrt::name_of<wgc::GraphicsCaptureSession>(), L"IsCursorCaptureEnabled")) {
      return std::unexpected(core::error("error.capture.cursor_control_unsupported"));
    }
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
    constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
    winrt::check_hresult(D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1,
        D3D11_SDK_VERSION, capture.device.put(), nullptr, capture.context.put()));
    if (auto initialized = comparison::initialize(state); !initialized) {
      failure = initialized.error();
    } else {
      auto dxgi = capture.device.as<IDXGIDevice>();
      winrt::com_ptr<IInspectable> inspectable;
      winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
      capture.runtime_device = inspectable.as<direct::Direct3D11::IDirect3DDevice>();
      auto interop =
          winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
      winrt::check_hresult(interop->CreateForWindow(
          hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(capture.item)));
      capture.pool_size = capture.item.Size();
      capture.pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
          capture.runtime_device, direct::DirectXPixelFormat::B8G8R8A8UIntNormalized,
          frame_pool_size, capture.pool_size);
      // 回调仅持有通知事件的独立句柄，不借用 AppState 或捕获资源。
      // 异步退订后的在途回调仍可安全结束，也不会唤醒下一次会话。
      capture.frame_token =
          capture.pool.FrameArrived([signal = std::move(callback_signal)](auto&&, auto&&) noexcept {
            SetEvent(signal.get());
          });
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
  if (auto stopped = stop(state); !stopped) {
    failure.related.push_back(stopped.error());
  }
  return std::unexpected(std::move(failure));
}

auto wait_stable_frame(core::AppState& state, std::int64_t after_source_time,
                       Clock::time_point deadline,
                       const std::function<std::expected<void, core::Error>()>& check,
                       bool require_change) -> std::expected<std::optional<Frame>, core::Error> {
  auto& capture = *state.capture;
  if (!capture.session) {
    return std::unexpected(core::error("error.capture.not_started"));
  }
  capture.comparison.stable_since_source = 0;
  capture.comparison.awaiting_change = require_change;
  while (true) {
    if (auto checked = check(); !checked) {
      return std::unexpected(checked.error());
    }
    if (!IsWindow(capture.hwnd)) {
      return std::unexpected(core::error("error.capture.window_closed"));
    }
    auto now = Clock::now();
    if (now >= deadline) {
      return std::optional<Frame>{};
    }
    // 定时醒来仅检查取消和窗口；没有帧通知时不访问帧池。
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
    // 停止请求与帧到达共用唤醒入口，先检查取消再取帧。
    if (auto checked = check(); !checked) {
      return std::unexpected(checked.error());
    }
    if (Clock::now() >= deadline) {
      return std::optional<Frame>{};
    }
    auto frame = read_frame(state, after_source_time, deadline, check);
    if (!frame) {
      return std::unexpected(frame.error());
    }
    if (!*frame) {
      continue;
    }
    if (auto checked = check(); !checked) {
      return std::unexpected(checked.error());
    }
    if (Clock::now() >= deadline) {
      return std::optional<Frame>{};
    }
    return std::move(**frame);
  }
}

auto wake(core::AppState& state) -> void {
  auto& capture = *state.capture;
  std::lock_guard lock(capture.signal_mutex);
  if (capture.frame_ready) {
    SetEvent(capture.frame_ready.get());
  }
}

auto stop(core::AppState& state) -> std::expected<void, core::Error> {
  auto& capture = *state.capture;
  std::expected<void, core::Error> result;
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
  if (capture.session) {
    close([&] { capture.session.Close(); });
  }
  if (capture.pool) {
    if (capture.frame_token) {
      close([&] { capture.pool.FrameArrived(*capture.frame_token); });
    }
    close([&] { capture.pool.Close(); });
  }
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
  {
    std::lock_guard lock(capture.signal_mutex);
    capture.frame_ready.close();
  }
  return result;
}

}  // namespace features::capture

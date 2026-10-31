#include "features/capture/capture.hpp"

#include "vendor/std.hpp"

#include "vendor/windows/d3d11.hpp"
#include "vendor/windows/dwmapi.hpp"
#include "vendor/windows/dxgi.hpp"
#include "vendor/windows/windows_graphics_capture_interop.hpp"
#include "vendor/windows/windows_graphics_directx_direct3d11_interop.hpp"
#include "vendor/windows/winrt/windows_foundation.hpp"
#include "vendor/windows/winrt/windows_graphics_capture.hpp"
#include "vendor/windows/winrt/windows_graphics_directx.hpp"
#include "vendor/windows/winrt/windows_graphics_directx_direct3d11.hpp"

#include "core/message/message.hpp"

namespace features::capture {

namespace wgc = winrt::Windows::Graphics::Capture;
namespace direct = winrt::Windows::Graphics::DirectX;

struct CaptureState {
  std::mutex mutex;
  HWND hwnd{};
  int width{};
  int height{};
  std::shared_ptr<const Frame> latest;
  core::Error error;
  std::uint64_t sequence{};
  bool stopping{};
  bool awaiting_target_size{};
  std::chrono::steady_clock::time_point last_copy{};
  winrt::com_ptr<ID3D11Device> device;
  winrt::com_ptr<ID3D11DeviceContext> context;
  winrt::com_ptr<ID3D11Texture2D> staging;
  direct::Direct3D11::IDirect3DDevice runtime_device{nullptr};
  winrt::Windows::Graphics::SizeInt32 pool_size{};
  wgc::GraphicsCaptureItem item{nullptr};
  wgc::Direct3D11CaptureFramePool pool{nullptr};
  wgc::GraphicsCaptureSession session{nullptr};
  winrt::event_token arrived{};
  winrt::event_token closed{};

  ~CaptureState() {
    try {
      if (pool) {
        pool.FrameArrived(arrived);
      }
      if (item) {
        item.Closed(closed);
      }
      if (session) {
        session.Close();
      }
      if (pool) {
        pool.Close();
      }
    } catch (...) {
      // 析构不抛出异常，捕获错误由显式操作路径报告。
    }
  }
};

namespace {

auto client_box(HWND hwnd, int frame_width, int frame_height, int width, int height)
    -> std::expected<std::optional<cv::Rect>, core::Error> {
  if (frame_width == width && frame_height == height) {
    return std::optional{cv::Rect(0, 0, width, height)};
  }
  POINT origin{};
  if (!ClientToScreen(hwnd, &origin)) {
    return std::unexpected(core::error("error.capture.client_origin_failed"));
  }
  std::vector<RECT> bounds;
  RECT window_rect{};
  if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &window_rect,
                                      sizeof(window_rect)))) {
    bounds.push_back(window_rect);
  }
  if (GetWindowRect(hwnd, &window_rect)) {
    bounds.push_back(window_rect);
  }
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

auto receive(const std::shared_ptr<CaptureState>& state,
             const wgc::Direct3D11CaptureFramePool& pool) -> void {
  std::scoped_lock lock(state->mutex);
  if (state->stopping || !state->error.message.key.empty()) {
    return;
  }
  try {
    auto frame = pool.TryGetNextFrame();
    if (!frame) {
      return;
    }
    auto now = std::chrono::steady_clock::now();
    if (now - state->last_copy < std::chrono::milliseconds(80)) {
      return;
    }
    auto content = frame.ContentSize();
    if (content.Width <= 0 || content.Height <= 0) {
      return;
    }
    if (state->awaiting_target_size &&
        (content.Width != state->pool_size.Width || content.Height != state->pool_size.Height)) {
      // WGC 可能仍持有调整前的缓冲尺寸；先归还旧帧，再按实际内容重建，不能读取裁断的图像。
      frame.Close();
      frame = nullptr;
      pool.Recreate(state->runtime_device, direct::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2,
                    content);
      state->pool_size = content;
      return;
    }
    auto access = frame.Surface()
                      .as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (content.Width > static_cast<int>(description.Width) ||
        content.Height > static_cast<int>(description.Height)) {
      return;
    }
    auto box = client_box(state->hwnd, content.Width, content.Height, state->width, state->height);
    if (!box) {
      state->error = box.error();
      return;
    }
    if (!*box) {
      if (!state->awaiting_target_size) {
        state->error = core::error("error.capture.client_region_unknown");
      }
      return;
    }
    if (state->staging) {
      D3D11_TEXTURE2D_DESC previous{};
      state->staging->GetDesc(&previous);
      if (previous.Width != description.Width || previous.Height != description.Height) {
        state->error = core::error("error.capture.size_changed");
        return;
      }
    } else {
      description.BindFlags = 0;
      description.MiscFlags = 0;
      description.Usage = D3D11_USAGE_STAGING;
      description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      winrt::check_hresult(
          state->device->CreateTexture2D(&description, nullptr, state->staging.put()));
    }
    state->context->CopyResource(state->staging.get(), texture.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(state->context->Map(state->staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
    cv::Mat bgr;
    try {
      cv::Mat bgra(static_cast<int>(description.Height), static_cast<int>(description.Width),
                   CV_8UC4, mapped.pData, mapped.RowPitch);
      cv::cvtColor(bgra(**box), bgr, cv::COLOR_BGRA2BGR);
    } catch (...) {
      state->context->Unmap(state->staging.get(), 0);
      throw;
    }
    state->context->Unmap(state->staging.get(), 0);
    state->latest = std::make_shared<Frame>(Frame{
        .sequence = ++state->sequence,
        .source_time = frame.SystemRelativeTime().count(),
        .received_at = now,
        .bgr = std::move(bgr),
    });
    state->awaiting_target_size = false;
    state->last_copy = now;
  } catch (const winrt::hresult_error& error) {
    state->error = core::with_detail(core::error("error.capture.receive_failed"),
                                     winrt::to_string(error.message()));
  } catch (const std::exception& error) {
    state->error = core::with_detail(core::error("error.capture.receive_failed"), error.what());
  } catch (...) {
    state->error = core::error("error.capture.callback_failed");
  }
}

}  // namespace

auto start(HWND hwnd, int width, int height) -> std::expected<Capture, core::Error> {
  try {
    if (!wgc::GraphicsCaptureSession::IsSupported()) {
      return std::unexpected(core::error("error.capture.unsupported"));
    }
    auto state = std::make_shared<CaptureState>();
    state->hwnd = hwnd;
    state->width = width;
    state->height = height;
    state->awaiting_target_size = true;
    winrt::check_hresult(D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
        D3D11_SDK_VERSION, state->device.put(), nullptr, state->context.put()));
    auto dxgi = state->device.as<IDXGIDevice>();
    winrt::com_ptr<IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
    state->runtime_device = inspectable.as<direct::Direct3D11::IDirect3DDevice>();
    auto interop =
        winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    winrt::check_hresult(interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                  winrt::put_abi(state->item)));
    state->pool_size = state->item.Size();
    state->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
        state->runtime_device, direct::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2,
        state->pool_size);
    std::weak_ptr<CaptureState> weak = state;
    state->arrived = state->pool.FrameArrived([weak](const auto& sender, const auto&) {
      if (auto state = weak.lock()) {
        receive(state, sender);
      }
    });
    state->closed = state->item.Closed([weak](const auto&, const auto&) {
      if (auto state = weak.lock()) {
        std::scoped_lock lock(state->mutex);
        state->error = core::error("error.capture.window_closed");
      }
    });
    state->session = state->pool.CreateCaptureSession(state->item);
    state->session.IsCursorCaptureEnabled(false);
    state->session.StartCapture();
    return Capture{std::move(state)};
  } catch (const winrt::hresult_error& error) {
    return std::unexpected(core::with_detail(core::error("error.capture.start_failed"),
                                             winrt::to_string(error.message())));
  }
}

auto snapshot(const Capture& capture) -> std::expected<std::shared_ptr<const Frame>, core::Error> {
  if (!capture.state) {
    return std::unexpected(core::error("error.capture.not_started"));
  }
  std::scoped_lock lock(capture.state->mutex);
  if (!capture.state->error.message.key.empty()) {
    return std::unexpected(capture.state->error);
  }
  return capture.state->latest;
}

auto stop(Capture& capture) -> void {
  auto state = std::move(capture.state);
  if (!state) {
    return;
  }
  {
    std::scoped_lock lock(state->mutex);
    state->stopping = true;
  }
  // 主线程先关闭会话，使最后一个回调不会承担 WinRT 资源析构。
  try {
    if (state->pool) {
      state->pool.FrameArrived(state->arrived);
    }
    if (state->item) {
      state->item.Closed(state->closed);
    }
    if (state->session) {
      state->session.Close();
    }
    if (state->pool) {
      state->pool.Close();
    }
  } catch (...) {
  }
  state->session = nullptr;
  state->pool = nullptr;
  state->item = nullptr;
}

}  // namespace features::capture

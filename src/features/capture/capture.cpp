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

namespace features::capture {
namespace wgc = winrt::Windows::Graphics::Capture;
namespace direct = winrt::Windows::Graphics::DirectX;
struct CaptureState {
  std::mutex mutex;
  HWND hwnd{};
  int width{};
  int height{};
  std::shared_ptr<const Frame> latest;
  std::string error;
  std::uint64_t sequence{};
  bool stopping{};
  std::chrono::steady_clock::time_point last_copy{};
  winrt::com_ptr<ID3D11Device> device;
  winrt::com_ptr<ID3D11DeviceContext> context;
  winrt::com_ptr<ID3D11Texture2D> staging;
  wgc::GraphicsCaptureItem item{nullptr};
  wgc::Direct3D11CaptureFramePool pool{nullptr};
  wgc::GraphicsCaptureSession session{nullptr};
  winrt::event_token arrived{};
  winrt::event_token closed{};
  ~CaptureState() {
    try {
      if (pool) pool.FrameArrived(arrived);
      if (item) item.Closed(closed);
      if (session) session.Close();
      if (pool) pool.Close();
    } catch (...) { /* 析构只释放WinRT资源，错误由显式捕获路径报告。 */
    }
  }
};
namespace {
auto client_box(HWND hwnd, int frame_width, int frame_height, int width, int height)
    -> std::expected<cv::Rect, std::string> {
  if (frame_width == width && frame_height == height) return cv::Rect(0, 0, width, height);
  POINT origin{};
  if (!ClientToScreen(hwnd, &origin)) return std::unexpected("读取客户区原点失败。");
  std::vector<RECT> bounds;
  RECT r{};
  if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
    bounds.push_back(r);
  if (GetWindowRect(hwnd, &r)) bounds.push_back(r);
  for (auto b : bounds) {
    int x = origin.x - b.left, y = origin.y - b.top;
    if (b.right - b.left == frame_width && b.bottom - b.top == frame_height && x >= 0 && y >= 0 &&
        x + width <= frame_width && y + height <= frame_height)
      return cv::Rect(x, y, width, height);
  }
  return std::unexpected("无法确定WGC中的客户区；请先检查 capture 截图。");
}
auto receive(const std::shared_ptr<CaptureState>& state,
             const wgc::Direct3D11CaptureFramePool& pool) -> void {
  std::scoped_lock lock(state->mutex);
  if (state->stopping || !state->error.empty()) return;
  try {
    auto frame = pool.TryGetNextFrame();
    if (!frame) return;
    auto now = std::chrono::steady_clock::now();
    if (now - state->last_copy < std::chrono::milliseconds(80)) return;
    auto content = frame.ContentSize();
    auto access = frame.Surface()
                      .as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (content.Width <= 0 || content.Height <= 0 || content.Width > static_cast<int>(desc.Width) ||
        content.Height > static_cast<int>(desc.Height))
      return;
    auto box = client_box(state->hwnd, content.Width, content.Height, state->width, state->height);
    if (!box) {
      state->error = box.error();
      return;
    }
    if (state->staging) {
      D3D11_TEXTURE2D_DESC previous{};
      state->staging->GetDesc(&previous);
      if (previous.Width != desc.Width || previous.Height != desc.Height) {
        state->error = "WGC捕获尺寸发生变化。";
        return;
      }
    } else {
      desc.BindFlags = 0;
      desc.MiscFlags = 0;
      desc.Usage = D3D11_USAGE_STAGING;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      winrt::check_hresult(state->device->CreateTexture2D(&desc, nullptr, state->staging.put()));
    }
    state->context->CopyResource(state->staging.get(), texture.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(state->context->Map(state->staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
    cv::Mat bgr;
    try {
      cv::Mat bgra(static_cast<int>(desc.Height), static_cast<int>(desc.Width), CV_8UC4,
                   mapped.pData, mapped.RowPitch);
      cv::cvtColor(bgra(*box), bgr, cv::COLOR_BGRA2BGR);
    } catch (...) {
      state->context->Unmap(state->staging.get(), 0);
      throw;
    }
    state->context->Unmap(state->staging.get(), 0);
    state->latest = std::make_shared<Frame>(
        Frame{++state->sequence, frame.SystemRelativeTime().count(), now, std::move(bgr)});
    state->last_copy = now;
  } catch (const winrt::hresult_error& e) {
    state->error = "WGC：" + winrt::to_string(e.message());
  } catch (const std::exception& e) {
    state->error = std::string("WGC：") + e.what();
  } catch (...) {
    state->error = "WGC回调失败。";
  }
}
}  // namespace
auto start(HWND hwnd, int width, int height) -> std::expected<Capture, std::string> {
  try {
    if (!wgc::GraphicsCaptureSession::IsSupported())
      return std::unexpected("当前Windows环境不支持WGC窗口捕获。");
    auto s = std::make_shared<CaptureState>();
    s->hwnd = hwnd;
    s->width = width;
    s->height = height;
    winrt::check_hresult(D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
        D3D11_SDK_VERSION, s->device.put(), nullptr, s->context.put()));
    auto dxgi = s->device.as<IDXGIDevice>();
    winrt::com_ptr<IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
    auto runtime_device = inspectable.as<direct::Direct3D11::IDirect3DDevice>();
    auto interop =
        winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    winrt::check_hresult(interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                  winrt::put_abi(s->item)));
    s->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
        runtime_device, direct::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, s->item.Size());
    std::weak_ptr<CaptureState> weak = s;
    s->arrived = s->pool.FrameArrived([weak](const auto& sender, const auto&) {
      if (auto state = weak.lock()) receive(state, sender);
    });
    s->closed = s->item.Closed([weak](const auto&, const auto&) {
      if (auto state = weak.lock()) {
        std::scoped_lock lock(state->mutex);
        state->error = "目标窗口已关闭。";
      }
    });
    s->session = s->pool.CreateCaptureSession(s->item);
    s->session.IsCursorCaptureEnabled(false);
    s->session.StartCapture();
    return Capture{std::move(s)};
  } catch (const winrt::hresult_error& e) {
    return std::unexpected("启动WGC失败：" + winrt::to_string(e.message()));
  }
}
auto snapshot(const Capture& capture) -> std::expected<std::shared_ptr<const Frame>, std::string> {
  if (!capture.state) return std::unexpected("WGC未启动。");
  std::scoped_lock lock(capture.state->mutex);
  if (!capture.state->error.empty()) return std::unexpected(capture.state->error);
  return capture.state->latest;
}
auto stop(Capture& capture) -> void {
  auto state = std::move(capture.state);
  if (!state) return;
  {
    std::scoped_lock lock(state->mutex);
    state->stopping = true;
  }
  // 主线程先关闭会话，使最后一个回调不会承担WinRT资源析构。
  try {
    if (state->pool) state->pool.FrameArrived(state->arrived);
    if (state->item) state->item.Closed(state->closed);
    if (state->session) state->session.Close();
    if (state->pool) state->pool.Close();
  } catch (...) {
  }
  state->session = nullptr;
  state->pool = nullptr;
  state->item = nullptr;
}
}  // namespace features::capture

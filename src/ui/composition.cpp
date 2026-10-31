#include "ui/composition.hpp"

#include "vendor/std.hpp"

#include "vendor/windows/d3d11.hpp"
#include "vendor/windows/dwrite.hpp"
#include "vendor/windows/dxgi.hpp"

#include "core/message/message.hpp"

#include "core/state/app_state.hpp"
#include "ui/main_window/state.hpp"
#include "ui/shared_render_resources/state.hpp"
#include "ui/status_window/state.hpp"

namespace ui::composition {
namespace {

auto checked(HRESULT result) -> std::expected<void, core::Error> {
  if (FAILED(result)) {
    return std::unexpected(core::error("error.ui.composition_failed",
                                       std::format("{:08X}", static_cast<unsigned long>(result))));
  }
  return {};
}

auto effect(core::AppState& state, Node& node) -> std::expected<void, core::Error> {
  if (node.effect) {
    return {};
  }
  Microsoft::WRL::ComPtr<IDCompositionEffectGroup> replacement;
  if (auto ready = checked(state.shared_render_resources->composition->CreateEffectGroup(
          replacement.GetAddressOf()));
      !ready) {
    return ready;
  }
  if (auto ready = checked(node.visual->SetEffect(replacement.Get())); !ready) {
    return ready;
  }
  node.effect = std::move(replacement);
  return {};
}

// DComp 管理返回的 D2D context 的 BeginDraw/EndDraw，不能再对 context 调用它们。
struct SurfaceUpdate {
  IDCompositionSurface* surface;
  Microsoft::WRL::ComPtr<ID2D1DeviceContext>& context;
  bool open{true};

  ~SurfaceUpdate() {
    if (open) {
      context.Reset();
      surface->EndDraw();
    }
  }
};

auto current(const Node& node, LARGE_INTEGER now) -> float {
  if (node.duration <= 0) {
    return node.to;
  }
  LARGE_INTEGER frequency{};
  QueryPerformanceFrequency(&frequency);
  double elapsed = static_cast<double>(now.QuadPart - node.started.QuadPart) / frequency.QuadPart;
  double t = std::clamp(elapsed / node.duration, 0.0, 1.0);
  // 与下方提交的 cubic ease-out 使用同一曲线；中途反向不跳回端点。
  double remaining = 1 - t;
  return node.to + (node.from - node.to) * static_cast<float>(remaining * remaining * remaining);
}

}  // namespace

auto initialize(core::AppState& state) -> std::expected<void, core::Error> {
  auto& shared = *state.shared_render_resources;
  if (shared.composition) {
    return {};
  }
  HRESULT result = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
      D3D11_SDK_VERSION, shared.d3d.GetAddressOf(), nullptr, nullptr);
  if (SUCCEEDED(result)) {
    result = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                               reinterpret_cast<void**>(shared.factory.GetAddressOf()));
  }
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
  if (SUCCEEDED(result)) {
    result = shared.d3d.As(&dxgi);
  }
  if (SUCCEEDED(result)) {
    result = shared.factory->CreateDevice(dxgi.Get(), shared.d2d.GetAddressOf());
  }
  if (SUCCEEDED(result)) {
    result = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(shared.write.GetAddressOf()));
  }
  if (SUCCEEDED(result)) {
    result = DCompositionCreateDevice2(shared.d2d.Get(), __uuidof(IDCompositionDesktopDevice),
                                       reinterpret_cast<void**>(shared.composition.GetAddressOf()));
  }
  if (FAILED(result)) {
    shared = {};
  }
  return checked(result);
}

auto attach(core::AppState& state, Node& node, IDCompositionVisual2* parent)
    -> std::expected<void, core::Error> {
  if (node.visual) {
    return {};
  }
  auto& device = state.shared_render_resources->composition;
  HRESULT result = device->CreateVisual(node.visual.GetAddressOf());
  if (SUCCEEDED(result) && parent) {
    // 无参考节点时 FALSE 才是最前层；按背景到前景的顺序构造子节点。
    result = parent->AddVisual(node.visual.Get(), FALSE, nullptr);
  }
  return checked(result);
}

auto create(core::AppState& state, Host& host, HWND hwnd) -> std::expected<void, core::Error> {
  if (host.target) {
    return {};
  }
  if (auto ready = initialize(state); !ready) {
    return ready;
  }
  if (auto ready = attach(state, host.root, nullptr); !ready) {
    return ready;
  }
  auto& device = state.shared_render_resources->composition;
  if (auto ready = checked(device->CreateTargetForHwnd(hwnd, TRUE, host.target.GetAddressOf()));
      !ready) {
    return ready;
  }
  if (auto ready = checked(host.target->SetRoot(host.root.visual.Get())); !ready) {
    return ready;
  }
  return attach(state, host.content.node, host.root.visual.Get());
}

auto draw(core::AppState& state, Layer& layer, UINT width, UINT height, UINT dpi,
          const std::function<void(ID2D1DeviceContext*, D2D1_MATRIX_3X2_F)>& paint)
    -> std::expected<void, core::Error> {
  width = std::max(1U, width);
  height = std::max(1U, height);
  if (!layer.surface || layer.width != width || layer.height != height || layer.dpi != dpi) {
    Microsoft::WRL::ComPtr<IDCompositionSurface> replacement;
    auto& device = state.shared_render_resources->composition;
    if (auto ready = checked(device->CreateSurface(width, height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                                   DXGI_ALPHA_MODE_PREMULTIPLIED,
                                                   replacement.GetAddressOf()));
        !ready) {
      return ready;
    }
    if (auto ready = checked(layer.node.visual->SetContent(replacement.Get())); !ready) {
      return ready;
    }
    layer.surface = std::move(replacement);
    layer.width = width;
    layer.height = height;
    layer.dpi = dpi;
  }
  Microsoft::WRL::ComPtr<ID2D1DeviceContext> context;
  POINT offset{};
  if (auto ready = checked(
          layer.surface->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext),
                                   reinterpret_cast<void**>(context.GetAddressOf()), &offset));
      !ready) {
    return ready;
  }
  SurfaceUpdate update{layer.surface.Get(), context};
  float scale = 96.0f / dpi;
  auto origin = D2D1::Matrix3x2F::Translation(offset.x * scale, offset.y * scale);
  context->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
  context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  context->SetTransform(origin);
  context->Clear(D2D1::ColorF(0, 0.0f));
  paint(context.Get(), origin);
  // 先释放 BeginDraw 返回的 context，再结束 Surface 更新。
  context.Reset();
  update.open = false;
  return checked(layer.surface->EndDraw());
}

auto place(Node& node, float x, float y, D2D1_RECT_F clip) -> std::expected<void, core::Error> {
  HRESULT result = node.visual->SetOffsetX(x);
  if (SUCCEEDED(result)) {
    result = node.visual->SetOffsetY(y);
  }
  if (SUCCEEDED(result)) {
    result = node.visual->SetClip(clip);
  }
  return checked(result);
}

auto animations_enabled() -> bool {
  BOOL enabled = TRUE;
  SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
  return enabled != FALSE;
}

auto opacity(core::AppState& state, Node& node, float value, double duration)
    -> std::expected<void, core::Error> {
  if (!animations_enabled()) {
    duration = 0;
  }
  if (!node.pulsing && node.to == value && (duration > 0 || node.duration == 0)) {
    return {};
  }
  if (auto ready = effect(state, node); !ready) {
    return ready;
  }
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  float from = current(node, now);
  HRESULT result = S_OK;
  if (duration <= 0) {
    duration = 0;
    result = node.effect->SetOpacity(value);
  } else {
    Microsoft::WRL::ComPtr<IDCompositionAnimation> animation;
    result = state.shared_render_resources->composition->CreateAnimation(animation.GetAddressOf());
    float delta = value - from;
    float time = static_cast<float>(duration);
    if (SUCCEEDED(result)) {
      result = animation->SetAbsoluteBeginTime(now);
    }
    if (SUCCEEDED(result)) {
      result = animation->AddCubic(0, from, 3 * delta / time, -3 * delta / (time * time),
                                   delta / (time * time * time));
    }
    if (SUCCEEDED(result)) {
      result = animation->End(duration, value);
    }
    if (SUCCEEDED(result)) {
      result = node.effect->SetOpacity(animation.Get());
    }
  }
  if (SUCCEEDED(result)) {
    node.from = from;
    node.to = value;
    node.duration = duration;
    node.started = now;
    node.pulsing = false;
  }
  return checked(result);
}

auto pulse(core::AppState& state, Node& node, bool active) -> std::expected<void, core::Error> {
  active = active && animations_enabled();
  if (!active) {
    return opacity(state, node, 1);
  }
  if (node.pulsing) {
    return {};
  }
  if (auto ready = effect(state, node); !ready) {
    return ready;
  }
  Microsoft::WRL::ComPtr<IDCompositionAnimation> animation;
  HRESULT result =
      state.shared_render_resources->composition->CreateAnimation(animation.GetAddressOf());
  if (SUCCEEDED(result)) {
    // 单个无结束的正弦段即可持续呼吸，不需要定时器或逐周期重新提交。
    result = animation->AddSinusoidal(0, 0.65f, 0.35f, 0.64f, 90);
  }
  if (SUCCEEDED(result)) {
    result = node.effect->SetOpacity(animation.Get());
  }
  if (SUCCEEDED(result)) {
    node.pulsing = true;
  }
  return checked(result);
}

auto commit(core::AppState& state) -> std::expected<void, core::Error> {
  return checked(state.shared_render_resources->composition->Commit());
}

auto recover(core::AppState& state) -> std::expected<bool, core::Error> {
  auto& shared = *state.shared_render_resources;
  if (!shared.d3d) {
    return false;
  }
  bool lost = FAILED(shared.d3d->GetDeviceRemovedReason());
  Microsoft::WRL::ComPtr<IDCompositionDevice> device;
  if (shared.composition && SUCCEEDED(shared.composition.As(&device))) {
    BOOL valid = TRUE;
    if (SUCCEEDED(device->CheckDeviceState(&valid)) && !valid) {
      lost = true;
    }
  }
  if (!lost) {
    return false;
  }
  auto& main = *state.main_window;
  auto& status = *state.status_window;
  HWND captured = GetCapture();
  bool button_capture =
      captured && ((captured == main.hwnd && main.buttons.pressed != button::Action::none) ||
                   (captured == status.hwnd && status.buttons.pressed != button::Action::none));
  main.buttons.pressed = button::Action::none;
  status.buttons.pressed = button::Action::none;
  if (button_capture) {
    ReleaseCapture();
  }
  main.buttons = {};
  main.composition = {};
  status.buttons = {};
  status.indicator = {};
  status.composition = {};
  device.Reset();
  shared = {};
  if (auto ready = initialize(state); !ready) {
    return std::unexpected(ready.error());
  }
  main.redraw = true;
  main.repaint_buttons = true;
  status.redraw = true;
  status.repaint_buttons = true;
  main.graphics_failed = false;
  status.graphics_failed = false;
  if (main.hwnd && !IsIconic(main.hwnd)) {
    InvalidateRect(main.hwnd, nullptr, FALSE);
  }
  if (status.hwnd && status.visible) {
    InvalidateRect(status.hwnd, nullptr, FALSE);
  }
  return true;
}

auto clear_backing(HWND hwnd, HDC dc) -> void {
  RECT client{};
  GetClientRect(hwnd, &client);
  FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
}

}  // namespace ui::composition

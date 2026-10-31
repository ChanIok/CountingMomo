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

// 校验 COM 返回值：失败时构造带十六进制错误码的 core::Error
auto checked(HRESULT result) -> std::expected<void, core::Error> {
  if (FAILED(result)) {
    return std::unexpected(core::error("error.ui.composition_failed",
                                       std::format("{:08X}", static_cast<unsigned long>(result))));
  }
  return {};
}

// 确保合成节点已绑定特效组：创建 IDCompositionEffectGroup 并关联至 Visual
auto effect(core::AppState& state, Node& node) -> std::expected<void, core::Error> {
  if (node.effect) {
    return {};
  }
  Microsoft::WRL::ComPtr<IDCompositionEffectGroup> replacement;
  // 创建合成特效组以承载透明度等视觉特效
  if (auto ready = checked(state.shared_render_resources->composition->CreateEffectGroup(
          replacement.GetAddressOf()));
      !ready) {
    return ready;
  }
  // 将特效组关联至节点的 Visual 实体
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

// 计算节点当前的实际不透明度：根据高精度时钟插值 cubic ease-out 曲线
auto current(const Node& node, LARGE_INTEGER now) -> float {
  if (node.duration <= 0) {
    return node.to;
  }
  LARGE_INTEGER frequency{};
  QueryPerformanceFrequency(&frequency);
  // 计算动画已流逝的时间占比
  double elapsed = static_cast<double>(now.QuadPart - node.started.QuadPart) / frequency.QuadPart;
  double t = std::clamp(elapsed / node.duration, 0.0, 1.0);
  // 与下方提交的 cubic ease-out 使用同一曲线；中途反向不跳回端点。
  double remaining = 1 - t;
  return node.to + (node.from - node.to) * static_cast<float>(remaining * remaining * remaining);
}

}  // namespace

// 初始化共享渲染资源：起 D3D11 设备 → 起 D2D 工厂与设备 → 起 DWrite → 起 DComposition 桌面设备
auto initialize(core::AppState& state) -> std::expected<void, core::Error> {
  auto& shared = *state.shared_render_resources;
  if (shared.composition) {
    return {};
  }
  // 建立硬件加速 D3D11 设备，开启 BGRA 交换链格式支持
  HRESULT result = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
      D3D11_SDK_VERSION, shared.d3d.GetAddressOf(), nullptr, nullptr);
  // 创建 Direct2D 单线程工厂
  if (SUCCEEDED(result)) {
    result = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                               reinterpret_cast<void**>(shared.factory.GetAddressOf()));
  }
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
  if (SUCCEEDED(result)) {
    result = shared.d3d.As(&dxgi);
  }
  // 关联 DXGI 设备创建 Direct2D 设备
  if (SUCCEEDED(result)) {
    result = shared.factory->CreateDevice(dxgi.Get(), shared.d2d.GetAddressOf());
  }
  // 创建共享 DirectWrite 字体工厂
  if (SUCCEEDED(result)) {
    result = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(shared.write.GetAddressOf()));
  }
  // 创建 DirectComposition 桌面级设备
  if (SUCCEEDED(result)) {
    result = DCompositionCreateDevice2(shared.d2d.Get(), __uuidof(IDCompositionDesktopDevice),
                                       reinterpret_cast<void**>(shared.composition.GetAddressOf()));
  }
  // 任一步骤异常则释放已分配的部分资源
  if (FAILED(result)) {
    shared = {};
  }
  return checked(result);
}

// 创建合成 Visual 节点并将其追加到父级视觉树的最前层
auto attach(core::AppState& state, Node& node, IDCompositionVisual2* parent)
    -> std::expected<void, core::Error> {
  if (node.visual) {
    return {};
  }
  auto& device = state.shared_render_resources->composition;
  // 创建轻量级 Visual 节点
  HRESULT result = device->CreateVisual(node.visual.GetAddressOf());
  if (SUCCEEDED(result) && parent) {
    // 无参考节点时 FALSE 才是最前层；按背景到前景的顺序构造子节点。
    result = parent->AddVisual(node.visual.Get(), FALSE, nullptr);
  }
  return checked(result);
}

// 为指定窗口建立 DirectComposition 视觉根宿主及内容层节点
auto create(core::AppState& state, Host& host, HWND hwnd) -> std::expected<void, core::Error> {
  if (host.target) {
    return {};
  }
  // 确保图形后端已就绪
  if (auto ready = initialize(state); !ready) {
    return ready;
  }
  // 创建根 Visual 容器
  if (auto ready = attach(state, host.root, nullptr); !ready) {
    return ready;
  }
  auto& device = state.shared_render_resources->composition;
  // 绑定 HWND 窗口句柄创建合成目标 Target
  if (auto ready = checked(device->CreateTargetForHwnd(hwnd, TRUE, host.target.GetAddressOf()));
      !ready) {
    return ready;
  }
  // 将根 Visual 挂载到宿主 Target
  if (auto ready = checked(host.target->SetRoot(host.root.visual.Get())); !ready) {
    return ready;
  }
  // 挂载内容渲染图层到根 Visual
  return attach(state, host.content.node, host.root.visual.Get());
}

// 在合成 Surface 上执行 Direct2D 离屏绘制：按需扩容 Surface → BeginDraw → 绘制回调 → EndDraw
auto draw(core::AppState& state, Layer& layer, UINT width, UINT height, UINT dpi,
          const std::function<void(ID2D1DeviceContext*, D2D1_MATRIX_3X2_F)>& paint)
    -> std::expected<void, core::Error> {
  width = std::max(1U, width);
  height = std::max(1U, height);
  // 当画布尺寸或屏幕 DPI 发生变动时重新分配合成表面
  if (!layer.surface || layer.width != width || layer.height != height || layer.dpi != dpi) {
    Microsoft::WRL::ComPtr<IDCompositionSurface> replacement;
    auto& device = state.shared_render_resources->composition;
    // 分配具有预乘 Alpha 的 BGRA8 表面
    if (auto ready = checked(device->CreateSurface(width, height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                                   DXGI_ALPHA_MODE_PREMULTIPLIED,
                                                   replacement.GetAddressOf()));
        !ready) {
      return ready;
    }
    // 将新表面更新为图层 Visual 的内容
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
  // 开启图层绘制并获取 Direct2D 设备上下文及绘制原点偏移
  if (auto ready = checked(
          layer.surface->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext),
                                   reinterpret_cast<void**>(context.GetAddressOf()), &offset));
      !ready) {
    return ready;
  }
  SurfaceUpdate update{layer.surface.Get(), context};
  // 配置 DPI 与原点坐标变换
  float scale = 96.0f / dpi;
  auto origin = D2D1::Matrix3x2F::Translation(offset.x * scale, offset.y * scale);
  context->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
  context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  context->SetTransform(origin);
  // 清理透明底色
  context->Clear(D2D1::ColorF(0, 0.0f));
  // 调用外部实际渲染过程
  paint(context.Get(), origin);
  // 先释放 BeginDraw 返回的 context，再结束 Surface 更新。
  context.Reset();
  update.open = false;
  return checked(layer.surface->EndDraw());
}

// 设置合成节点的二维坐标偏移与矩形裁剪区域
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

// 查询系统级客户区动画设置（检查系统是否开启了动画效果）
auto animations_enabled() -> bool {
  BOOL enabled = TRUE;
  SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
  return enabled != FALSE;
}

// 对合成节点的不透明度应用缓动动画或即时变更：计算起始值 → 构造三次贝塞尔动画 → 提交至特效组
auto opacity(core::AppState& state, Node& node, float value, double duration)
    -> std::expected<void, core::Error> {
  // 系统禁用动效时立即完成
  if (!animations_enabled()) {
    duration = 0;
  }
  // 目标值已匹配且非脉动状态则无需重复开启动画
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
  // 无持续时间时直接设置静态透明度
  if (duration <= 0) {
    duration = 0;
    result = node.effect->SetOpacity(value);
  } else {
    // 构造 Cubic Ease-Out 贝塞尔曲线过渡动画
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
  // 记录本次动画的起点与参数
  if (SUCCEEDED(result)) {
    node.from = from;
    node.to = value;
    node.duration = duration;
    node.started = now;
    node.pulsing = false;
  }
  return checked(result);
}

// 控制合成节点的正弦呼吸脉动特效：创建无限循环正弦动画或停止并恢复完全不透明
auto pulse(core::AppState& state, Node& node, bool active) -> std::expected<void, core::Error> {
  active = active && animations_enabled();
  // 停用脉动时平滑恢复至不透明状态
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

// 提交当前设备上所有合成树变更以使其在屏幕上原子呈现
auto commit(core::AppState& state) -> std::expected<void, core::Error> {
  return checked(state.shared_render_resources->composition->Commit());
}

// 显卡驱动崩溃或设备丢失后的渲染资源重建：检测丢失 → 释放句柄与捕获 → 重启 D3D/D2D/DComp →
// 标记窗口全量重绘
auto recover(core::AppState& state) -> std::expected<bool, core::Error> {
  auto& shared = *state.shared_render_resources;
  if (!shared.d3d) {
    return false;
  }
  // 探测底层 D3D11 设备是否被移除
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
  // 释放被捕获的鼠标输入
  HWND captured = GetCapture();
  bool button_capture =
      captured && ((captured == main.hwnd && main.buttons.pressed != button::Action::none) ||
                   (captured == status.hwnd && status.buttons.pressed != button::Action::none));
  main.buttons.pressed = button::Action::none;
  status.buttons.pressed = button::Action::none;
  if (button_capture) {
    ReleaseCapture();
  }
  // 清理受损的图形与视觉树句柄
  main.buttons = {};
  main.composition = {};
  status.buttons = {};
  status.indicator = {};
  status.composition = {};
  device.Reset();
  shared = {};
  // 重新建立 D3D/D2D/DComp 共享设备栈
  if (auto ready = initialize(state); !ready) {
    return std::unexpected(ready.error());
  }
  // 标记窗口需要全量重刷
  main.redraw = true;
  main.repaint_buttons = true;
  status.redraw = true;
  status.repaint_buttons = true;
  main.graphics_failed = false;
  status.graphics_failed = false;
  // 触发窗口重绘
  if (main.hwnd && !IsIconic(main.hwnd)) {
    InvalidateRect(main.hwnd, nullptr, FALSE);
  }
  if (status.hwnd && status.visible) {
    InvalidateRect(status.hwnd, nullptr, FALSE);
  }
  return true;
}

// 清理窗口 GDI 背景底色为全黑以配合 DirectComposition 透明合成
auto clear_backing(HWND hwnd, HDC dc) -> void {
  RECT client{};
  GetClientRect(hwnd, &client);
  FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
}

}  // namespace ui::composition

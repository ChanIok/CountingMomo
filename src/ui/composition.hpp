#pragma once

#include "vendor/std.hpp"

#include "vendor/windows.hpp"
#include "vendor/windows/d2d1_1.hpp"
#include "vendor/windows/dcomp.hpp"
#include "vendor/windows/wrl/client.hpp"

#include "core/message/message.hpp"

namespace core {
struct AppState;
}

namespace ui::composition {

struct Node {
  Microsoft::WRL::ComPtr<IDCompositionVisual2> visual;
  Microsoft::WRL::ComPtr<IDCompositionEffectGroup> effect;
  float from{1};
  float to{1};
  double duration{};        // 秒；仅在反向切换时计算当前透明度，不逐帧采样。
  LARGE_INTEGER started{};  // 与 DComp absolute begin time 共用 QPC 时间轴。
  bool pulsing{};
};

struct Layer {
  Node node;
  Microsoft::WRL::ComPtr<IDCompositionSurface> surface;
  UINT width{};  // Surface 原生像素尺寸。
  UINT height{};
  UINT dpi{};
};

struct Host {
  Microsoft::WRL::ComPtr<IDCompositionTarget> target;
  Node root;
  Layer content;
};

auto initialize(core::AppState& state) -> std::expected<void, core::Error>;
auto create(core::AppState& state, Host& host, HWND hwnd) -> std::expected<void, core::Error>;
// 新节点放在已有兄弟节点前方；调用者按背景到前景构造。parent 为空时只创建根节点。
auto attach(core::AppState& state, Node& node, IDCompositionVisual2* parent)
    -> std::expected<void, core::Error>;

// 每次完整更新这一层；回调使用 DIP，origin 包含 DComp 分配的非零绘制偏移。
// 一次只打开一个 Surface，异常离开也会 EndDraw；本函数不提交合成事务。
auto draw(core::AppState& state, Layer& layer, UINT width, UINT height, UINT dpi,
          const std::function<void(ID2D1DeviceContext*, D2D1_MATRIX_3X2_F)>& paint)
    -> std::expected<void, core::Error>;
auto place(Node& node, float x, float y, D2D1_RECT_F clip) -> std::expected<void, core::Error>;
auto opacity(core::AppState& state, Node& node, float value, double duration = 0)
    -> std::expected<void, core::Error>;
auto pulse(core::AppState& state, Node& node, bool active) -> std::expected<void, core::Error>;
auto commit(core::AppState& state) -> std::expected<void, core::Error>;
// 仅在设备确已失效时重建两窗的派生图形资源；业务状态与扫描设备不受影响。
auto recover(core::AppState& state) -> std::expected<bool, core::Error>;
auto animations_enabled() -> bool;
auto clear_backing(HWND hwnd, HDC dc) -> void;

}  // namespace ui::composition

#pragma once

#include "vendor/std.hpp"

#include "vendor/windows/d3d11.hpp"

#include "core/message/types.hpp"

namespace core {
struct AppState;
}

namespace features::scan::capture::comparison {

// 初始化 GPU 比对子系统：编译计算着色器 → 创建参考纹理、常量缓冲区与查询事件
auto initialize(core::AppState& state) -> std::expected<void, core::Error>;

// 执行帧比对与参考帧更新：调度着色器比对差分 → 读回结果 → 若变化则拷贝当前帧为新参考
auto update(core::AppState& state, ID3D11Texture2D* source, const RECT& client,
            std::chrono::steady_clock::time_point deadline,
            const std::function<std::expected<void, core::Error>()>& check)
    -> std::expected<std::optional<bool>, core::Error>;

// 等待已提交的 GPU 比对与拷贝命令执行完成
auto finish(core::AppState& state, std::chrono::steady_clock::time_point deadline,
            const std::function<std::expected<void, core::Error>()>& check)
    -> std::expected<bool, core::Error>;

}  // namespace features::scan::capture::comparison

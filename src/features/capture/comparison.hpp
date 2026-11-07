#pragma once

#include "vendor/std.hpp"

#include "vendor/windows/d3d11.hpp"

#include "core/message/types.hpp"

namespace core {
struct AppState;
}

namespace features::capture::comparison {

auto initialize(core::AppState& state) -> std::expected<void, core::Error>;

// 使用当前自有工作纹理，与唯一的自有参考纹理比较；首帧或不同时才更新参考。
// 空值表示到 deadline 仍未得出比较结果，由调用方按超时处理。
auto update(core::AppState& state, ID3D11Texture2D* source, const RECT& client,
            std::chrono::steady_clock::time_point deadline,
            const std::function<std::expected<void, core::Error>()>& check)
    -> std::expected<std::optional<bool>, core::Error>;

// 归还 WGC 帧前等已提交的 GPU 命令完成。取消优先交付错误；false 表示到 deadline 未完成。
auto finish(core::AppState& state, std::chrono::steady_clock::time_point deadline,
            const std::function<std::expected<void, core::Error>()>& check)
    -> std::expected<bool, core::Error>;

}  // namespace features::capture::comparison

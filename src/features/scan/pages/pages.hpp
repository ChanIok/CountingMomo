#pragma once

// 页面扫描领域接口：会话状态、视觉识别与对外采集入口
#include "vendor/std.hpp"

#include "features/scan/pages/types.hpp"
#include "features/scan/pages/vision.hpp"

namespace core {
struct AppState;
}

namespace features::scan::pages {

// 加载问号模板并初始化页面域资源；失败即中断本次扫描。
auto begin(core::AppState& state) -> void;

// 采集当前分类的全部页面；翻页接续、滚轮标定与重叠行去重都在实现内部。
auto read_all_pages(core::AppState& state) -> std::vector<int>;

}  // namespace features::scan::pages

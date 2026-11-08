[English](README_EN.md) | [简体中文](README.md)

# CountingMomo

《无限暖暖》服装数据导出工具。一键扫描游戏内图鉴，快速导出你的衣橱收集进度。

## 简介

- **全自动**：自动切换服装分类并逐页滚动翻页扫描

- **纯视觉识别**：纯屏幕捕获与视觉检测，不读写游戏内存

- **轻量原生**：纯 C++ 构建，GPU 加速画面捕获，打包仅约 1 MB

## 使用方法

> **运行环境**：Windows 10 2004 或更高版本。

1. 在游戏内打开 **美鸭梨 → 博物图鉴 → 检索**。
2. 打开程序，点击主面板上的 **开始扫描**（默认按 **F10** 停止）。
3. 扫描完成后，点击 **复制结果**（可直接粘贴到 [NUAN5.PRO](https://nuan5.pro/data) 中）。

## 结果说明

导出的数据为各分类的拥有状态（`1` 为已拥有，`0` 为未拥有），严格按游戏内图鉴顺序排列，可直接供社区工具解析配对：

```json
{
  "hair": [0, 1, 0, 1],
  "dresses": [1, 0, 0, 1]
}
```

## 构建与开发

开发规范与架构说明见 [AGENTS.md](AGENTS.md)。

```powershell
pnpm run build --release     # 构建 Release
pnpm run package             # 打包便携 zip
```

## 技术栈

C++23 · clang-cl · xmake · OpenCV · reflect-cpp · Win32 · Direct2D · Direct3D 11 · Windows.Graphics.Capture

## 许可证

本项目基于 [MIT License](LICENSE) 开源。项目 Logo 和游戏相关素材版权归《无限暖暖》开发商所有。

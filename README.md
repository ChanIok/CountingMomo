# 盘点吧大喵 · CountingMomo

《无限暖暖》Windows 图鉴拥有状态采集工具，自动切换分类、滚动读取，生成可复制到其他应用的 JSON。

> 当前版本仍在开发中，完整扫描流程尚未完成实机验证。

## 使用

需要 Windows 10 2004 或更高版本，以及支持 WGC 和 D3D11 的环境。
解压后保留 `CountingMomo.exe` 与 `resources/` 的相对位置，运行程序。

1. 在游戏中打开 **美鸭梨 → 博物图鉴 → 检索**，清除筛选并使用默认排序。
2. 收起左侧的 **饰品**、**妆容** 分组，将分类栏滚到顶部。
3. 点击 **开始扫描**。程序会最小化主面板，激活游戏并依次扫描 27 个分类；采集范围不包含套装、全妆和肤色。
4. 完成后点击 **复制结果**，或选择 **另存 JSON**。关闭程序前请取走结果。

扫描会临时将游戏调整为 **1440×5760 无边框竖屏**，默认在结束后恢复原窗口。
分类菜单上部（客户区 y=728 以内）须在显示器内且无遮挡。扫描期间不要手动操作游戏或切走焦点；按 **F8** 停止，关闭程序会先等待任务收尾。

结果只保留在当前进程内，不自动保存业务文件或覆盖剪贴板。失败或停止时不提供本次完整结果，
此前的成功结果仍可通过 **复制上次成功结果** 取回。

## 结果

```json
{
  "hair": [0, 1, 0, 1],
  "dresses": [1, 0, 0, 1]
}
```

`1` 为拥有，`0` 为未拥有，问号物品记 0 并保留位置。每个数组从分类第一件开始，逐行从左到右排列；
接收方用同版本、同分类、按图鉴默认顺序排列的物品 ID 表配对。扫描器不读取数据库或识别名称。
英文分类键见 [categories.json](resources/categories/categories.json) 中各组 `categories` 条目的 `id`。

## 问题排查

- 无法自动激活时，在提示期限内 Alt+Tab 切回游戏；游戏以管理员权限运行时，本工具也需相同权限。
- 失败原因在主面板显示，诊断保存在 `%LOCALAPPDATA%/CountingMomo/runs/<时间戳>/diagnostics/`。
- 窗口恢复失败时，通过游戏设置恢复窗口模式与尺寸；已完整取得的结果仍可复制。

设置保存在 `%LOCALAPPDATA%/CountingMomo/settings.json`。

语言默认按 Windows 显示语言选择：中文使用简体中文，其余使用 English；可在设置中切换并保存。

## 构建

需要 clang-cl、支持 C++23 的 MSVC 标准库、Windows SDK、xmake、vcpkg 和 Node.js。在仓库根目录执行：

```powershell
xmake f -p windows -a x64 -m release
xmake build CountingMomo
xmake package                # 仅打包已有 Release，不触发构建
```

程序输出到 `build/windows/x64/release/`，发行包输出到 `dist/CountingMomo-<时间戳>/`。
程序与布局、素材须一起更新；对外分发前确认所附游戏界面素材的使用与再分发权限。

开发约定见 [AGENTS.md](AGENTS.md)，文本维护与 C++ 用法见 [i18n 开发说明](docs/I18N.md)。

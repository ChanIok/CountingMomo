# 数数吧大喵 · CountingMomo

《无限暖暖》图鉴扫描与统计命令行工具，适用于 Windows。

- 自动调整游戏窗口，捕获并滚动采集当前分类。
- 使用游戏字体和名称库匹配完整名称，判断物品拥有状态。
- 导出 JSON、CSV 和带图片证据、名称候选的 HTML 核对报告。
- 原生 C++ 运行，随程序携带名称 TXT，不需要 Python 或 OCR 模型。

目前处于开发阶段。默认布局为 1440×5760，已完成静态截图标定及用户提供的连衣裙679项到底扫描，锚点 ±1px 修复已在该次实机记录中生效。当前新增的 JPG 页面保存、识别计算优化及无滚动条单屏适配仅完成源码编辑，尚未构建或运行验证。

## 开始扫描

运行要求：Windows 10 2004 或更高版本，支持 WGC 窗口捕获的系统和可用的 D3D11 设备。

1. 打开游戏图鉴，手动选择分类、清除筛选并滚到最顶部，保持窗口模式。
2. 在程序所在目录打开终端，执行：

   ```powershell
   .\CountingMomo.exe scan --category 发型 --expected-count 820
   ```

3. 保持游戏前台，等待结束后打开结果目录中的 `review.html`。

`--category` 只是结果标签，不会在游戏中选择分类，也不用于筛选名称库。`--expected-count` 可省略；填写时应包含未拥有物品，用于滚动条到底或单屏采集结束后的数量核对，不会在达到数量时提前停止。

默认按整名分数及候选差距接受名称，不做单字低分核对，也不生成逐字证据。需要排查字形时，添加无值开关 `--validate-characters`（同时适用于 `scan` 和 `analyze`）：

```powershell
.\CountingMomo.exe scan --category 发型 --validate-characters
.\CountingMomo.exe analyze --image .\首屏.jpg --category 发型 --validate-characters
```

开启后，单字分数低于0.9会标记待核对，但不会否决已通过整名条件的名称；逐字证据写入 `name.characters` 并在报告中展示。关闭时跳过跨字号逐字分数保存、证据整理及低分提示生成；整名评分本身仍需计算字形分数，不能据此假定主要匹配计算量减少。此开关已完成源码修改，尚未构建或运行验证，实际耗时差异需同批材料对比。

程序会将游戏调整为 **1440×5760 无边框竖屏**，主屏水平居中、顶边对齐，退出后保留该窗口状态。若自动激活失败，请在 30 秒内 Alt+Tab 切回游戏。游戏以管理员运行时，终端也需要相同权限。

扫描中不要手动滚动、切换分类或改变窗口尺寸。按 **F8 / Ctrl+C** 停止采集，程序会处理完已入队内容，再导出部分结果。失焦、窗口关闭、超时或接续失败也会停止并保留已有结果。

### 指定窗口与结果位置

```powershell
# 列出窗口，使用句柄明确指定目标
.\CountingMomo.exe list-windows
.\CountingMomo.exe scan --category 发型 --hwnd 0x14b0c4e

# 自定义结果目录，必须是尚不存在的目录
.\CountingMomo.exe scan --category 发型 --output .\results\hair

# 只保存当前客户区，不调整窗口、激活游戏或发送输入，也不加载字体或名称库
.\CountingMomo.exe capture

# 查看全部参数，包括字体、名称库、布局、滚动幅度和超时设置
.\CountingMomo.exe --help
```

默认按进程名 `X6Game-Win64-Shipping.exe` 定位游戏窗口：进程快照精确匹配后，取该进程可见、无 owner、面积最大的顶层窗口。标题不参与匹配，浏览器等其他窗口不会干扰。游戏可执行文件改名时用 `--exe` 覆盖，同一进程下窗口选择有歧义时仍可用 `--hwnd` 明确指定。结果默认写入当前工作目录的 `results/<时间戳>/`。默认字体、名称库和布局相对于可执行文件读取；显式参数路径相对于当前工作目录。

### 不启动游戏，核对原始截图

构建新版本后，可直接分析与布局尺寸一致的原始客户区截图，不查找窗口、不启动捕获，也不调整窗口或发送输入：

```powershell
# 分类首屏，按默认首行位置裁切
.\CountingMomo.exe analyze --image .\首屏.jpg --category 发型

# 已滚动截图，显式指定行相位；本次到底样本的行距为132，行相位为10
.\CountingMomo.exe analyze --image .\到底.jpg --category 发型 --row-phase 10
```

`--row-phase` 是卡片行首 y 坐标对布局行距的余数，取值为 `0` 至 `row_step-1`，并使分析避开滚动后的顶部渐隐区域。实时扫描由图像锚点维护行相位，不需要此参数。离线分析不缩放图片，不核对分类总数，只导出单图安全区域内的名称、拥有状态和三类图片证据；行号是图内顺序。结果状态为 `image_analyzed`，`session.source_image` 记录源图；不生成滚动事务。

## 查看结果

- **`review.html`**：先看此文件，核对名称、拥有状态、原图及不确定原因。
- **`items.json` / `items.csv`**：用于统计或后续处理；JSON 中名称、完整名称候选位于 `name` 对象；开启单字校验时 `name.characters` 保存最佳候选的逐字证据，默认是空数组。未确定名称的 `name.text` 为空，`name.candidates` 保留最佳候选。
- `items.jsonl`：后台逐项写入、按批次刷新，保留中断前已写入的条目。
- `session.json`：结束状态、采集与处理数量、滚动标定、名称库路径与候选数量、排除名称、补字字体及字符、单字校验开关（`validate_characters`）、采集识别阶段耗时（`processing_seconds`）和总耗时。
- `timings.json`：采集识别阶段（`processing_wall_seconds`）、报告阶段裁图及存图（`crop_export_seconds`）、报告导出（`report_seconds`），以及识别分项耗时、查询位置数、实际字形比较次数。采集识别阶段包含启动准备、页面保存及后台排空；总耗时包含后续报告阶段。控制台也会打印汇总；各项存在包含和线程重叠关系，不能直接相加。
- `transactions.jsonl`：逐次滚动输入、帧时间、锚点分数和就绪过程。锚点 `score`、`weakest_score`、`runner_up` 使用每张名称纵向 ±1px 对齐后的分数；`raw_score` 和 `raw_weakest_score` 保留严格对齐时的平均分、最差分。`samples` 逐张记录锚点内的行列（从1开始）、`vertical_offset` 及校正前后分数；这些局部偏移不改变滚动位移或页面坐标。
- `frames/` / `crops/`：页面及逐项图片证据。扫描中将完整页面保存为质量100的 `frames/fXXXX.jpg`，识别、拥有判定及接续仍使用未压缩的内存像素。识别结束后，报告阶段按页读取 JPG 并生成卡片、名称和星星 PNG 裁图，每次只持有一页，不累积整次扫描图片。JPG质量100仍有压缩损失，报告裁图与识别时的像素并非完全一致。到底诊断 `end_frame.png`、失败诊断 `unmatched_frame.png` 及单次 `capture.png` 保持无损PNG。

退出码：成功到底、单屏采集完成、单次捕获或离线分析为 `0`，失败为 `1`，用户停止或达到滚动次数上限为 `2`。

### 当前识别范围

自定义布局需填写与columns列数一致的columns_without_scrollbar；程序与布局资源须一同更新。

首屏未检测到滚动条时，按当前分类能在一屏完整展示处理，自动使用无滚动条列坐标304/672/1040，允许卡片后的连续空位；保留现有名称、星星及页面结构稳定等待。稳定后提交首屏并结束，不发送滚动输入，不需要新增参数或指定特殊分类名。扫描状态为single_page_complete；指定总数且吻合时为count_verified。session.single_page记录该模式，items.json中的layout.columns保存实际使用的列坐标。普通页面继续使用304/669/1033及原滚动条到底流程。

analyze分析首屏时也自动选择这套列坐标，仍返回image_analyzed；传入--row-phase的滚动截图不走单屏判断。当前按“无滚动条即单屏”简化假设实现，没有自动几何校准；Python已核对肤绘31项截图，C++尚未构建或运行验证。

默认布局针对 **1440×5760、三列固定网格**标定，其他分类需要具有相同几何布局。名称裁图为 **155×30px**，追踪区域独立为 **150×30px**。裁图右界位于徽标左侧，完整裁图参与匹配，不使用图标遮挡掩码。

普通字号与小字号共用名称库灰度匹配。短名称直接使用 20px；文字跨度达到 `裁图宽度−最大字号`（默认 135px）时，按笔画高度限定少量邻近字号，再比较完整名称得分。字号范围为 14～20px，模板参考基线为 22px，整行投影定位 ±1px 垂直偏移；结果的 `name.baseline` 记录裁图内实际使用的基线。不同字号或起点的同一名称只算一个候选。

布局中的 `name_recognition` 集中指定 `font_size_range`、`baseline` 和 `origin_count`；裁图高度、模板及字宽缓存按布局建立，不再固定为旧分辨率的 20px 高、五档字号。自定义布局也需填写这些标定量。新布局须与新程序一起使用，不能只把新 JSON 复制给旧可执行文件。

名称相似度为裁图内字符灰度余弦值的平均，候选差距比较两个不同的完整名称；两者都不是正确概率。整名分数低于0.9或候选差距低于0.01时名称留空，不强行填入最近邻。单字低分核对默认关闭，添加 `--validate-characters` 后才标记待核对并输出最佳候选的逐字证据，不否决整名。无论是否开启，候选名称超出裁图、存在不可见字符或使用补字字体时仍提示核对。拥有状态依据最右侧星星的 **18 个固定前景像素**判断，掩码与原生裁图同尺寸，不缩放采样。

**数量吻合不代表名称全部正确，待核对也不等于识别错误。** 请以报告中的原图和候选为依据。

## 名称库

默认读取 `resources/clothes_names.txt`，可用 `--names PATH` 指定另一份 UTF-8 TXT，一行一个名称。当前包含从根目录 `clothes_all.json` 的 `clothes[*].name` 提取的 **9,624 个名称**，仅去首尾空白、空名称和精确重复项。内部空格、标点、套装及疑似占位名称保留，不读取 `clothes_set`、`pose` 或 `colors`。

更新源 JSON 后，在项目根目录重新提取（仅开发时需要 Python）：

```powershell
python scripts/extract_clothes_names.py
```

运行时只读 TXT，不加载原始 JSON。识别只比较名称库实际使用的字符，同一字号、同一位置的同一字形只比较一次，向量长度按实际比较字宽预先缓存，比较时只求点积，再汇总完整名称得分；不使用自由认字或编辑距离纠错后备流程。空格和标点按字体实际字宽排布。

识别计算使用x64基线SSE2点积及可复用字格缓冲区；查询向量不再先整体归一化，直接使用实际可见字宽的范数计算余弦分数。模板仍保留原有归一化及投影定位。各字号按名称宽度建立索引，沿用原宽度筛选范围，再保留五个不同名称的最佳结果，不完整排序全部候选。字号、边界处理、评分公式及接受阈值不变；浮点舍入变化须通过同批原始裁图核对。

“遇上喵喵雨・星彩”已确认不在扫描范围内：TXT 保留原文，加载时排除整条名称，当前实际参与匹配 **9,623 个名称**；不把 `・` 替换成其他中点。其他混杂条目留待后续清洗。

字体优先使用游戏字体；缺字时才读取系统 Windows 目录下的 `Fonts/msyh.ttc`（常规微软雅黑）补字，当前用于 `湲`。其他字符的模板不变，两种字体共用行基线。启动日志和 `session.json` 记录排除名称与补字字符，含补字的识别结果标记待核对。系统微软雅黑不会复制到项目或分发目录。

名称库需要随游戏内容更新；库外名称可能没有可靠匹配，须检查低分与歧义结果。名称库无效、需要补字但系统微软雅黑不可用、或两个字体均缺字时，会明确报错。

## 从源码构建

需要 clang-cl、支持 C++23 的 MSVC 标准库、Windows SDK、xmake 和 vcpkg。构建风格参考 SpinningMomo，OpenCV 的精简依赖配置参考 SlackingMomo。

先将游戏 TrueType 字体放到 `resources/fonts/FZYASHJW_Zhong.ttf`。字体不纳入源码跟踪，源码检出后需自行准备；运行时也可通过 `--font PATH` 指定，不需要安装系统字体。

在项目根目录执行：

```powershell
xmake f -p windows -a x64 -m release
xmake build CountingMomo

.\build\windows\x64\release\CountingMomo.exe scan --category 发型
```

依赖由 vcpkg 引入，使用固定 baseline 与 xmake 锁文件：OpenCV（关闭默认功能，使用 core/imgproc）、FreeType、reflect-cpp。图片写入使用 Windows WIC。Release 使用静态第三方依赖。

构建后会将布局、名称库及已有字体复制到可执行文件旁的 `resources/`。程序不会自动寻找开发机字体目录。

## 便携打包

准备好已有的 Release 构建和字体后执行：

```powershell
xmake package
```

该任务不重新构建，输出 `dist/CountingMomo-<时间戳>/`：

```text
CountingMomo.exe
README.md
resources/
├─ layout.json
├─ clothes_names.txt
└─ fonts/
   └─ FZYASHJW_Zhong.ttf
```

保持目录结构即可运行，无需 Python、模型下载或安装器。**对外分发前须确认字体资源的使用与再分发授权。**

## 开发说明

`src/features/scan/usecase.cpp` 编排扫描、后台队列和收尾；窗口、捕获、视觉判断、名称识别、报告各自独立。代码采用结构体与自由函数、`std::expected`、UTF-8 内部字符串和自包含头文件。

本次分辨率标定的方法、逐项标签、离线结果位置和复现命令见 [1440×5760 标定记录](prototype/experiments/RESOLUTION_1440.md)。Python 仅用于开发期实验，不参与 C++ 运行及打包。

连衣裙最后一页的锚点偏移修复及保存页面回放结果见 [锚点局部对齐记录](prototype/experiments/ANCHOR_ALIGNMENT.md)。

以采集/识别阶段为目标的 JPG 保存及计算优化见 [识别阶段优化记录](prototype/experiments/RECOGNITION_PERFORMANCE.md)。

开发约束与扫描不变量见 [AGENTS.md](AGENTS.md)。[prototype/](prototype/README.md) 保留 Python 原型与对照材料，C++ 运行及分发不依赖它。

按开发约定，AI 不自动构建、运行测试或操作游戏，需由用户确认或手动执行。

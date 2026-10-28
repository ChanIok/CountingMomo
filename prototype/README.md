# CountingMomo prototype

Windows 图鉴扫描原型：WGC 捕获、逐屏滚动、游戏字体逐字灰度匹配、星形前景拥有状态判定，以及 CSV/JSON/HTML 导出。

名称识别已统一到 `recognition.py`，普通字号和小字号使用同一条流程。离线、实时扫描及当前验证脚本共用该模块；运行时不读取服装名称库，不加载 PaddleOCR，也不使用汉明粗筛或其他识别后备路径。

## 安装

Windows 10/11、Python 3.11，基础依赖为 NumPy、OpenCV 和 Pillow。WGC 单独安装：

```powershell
cd D:\code\CountingMomo\prototype
uv venv --python 3.11 .venv
uv pip sync --python .venv\Scripts\python.exe requirements.txt
uv pip install --python .venv\Scripts\python.exe --no-deps -r requirements-wgc.txt
```

WGC 使用 `--no-deps`，因为现有 `opencv-contrib-python` 已提供它需要的 `cv2` API，避免重复安装 `opencv-python`。同步基础依赖后须重新安装 WGC。本次只更新依赖清单，没有修改现有虚拟环境中的包。

字体默认读取 `../temp/FZYASHJW_Zhong.ttf`，不复制、不安装到系统，不随代码分发。可用 `--font <路径>` 指定游戏 TrueType 字体。没有模型下载。

依赖变动后重新生成锁定文件：

```powershell
uv pip compile requirements.in --python .venv\Scripts\python.exe --output-file requirements.txt
```

## 实时扫描

手动打开分类、设置筛选并滚到顶部，然后执行：

```powershell
.\.venv\Scripts\python.exe live_scan.py --category '发型' --expected-count 820
```

`--expected-count` 可省略，统计包含未拥有物品。程序不切换分类，`--category` 是结果标签。

启动时按布局设置无边框960×3840窗口，在主屏水平居中、顶边对齐主屏。窗口样式沿用 SpinningMomo 的 `WS_POPUP` 做法，不修改显示器分辨率。确认窗口位置与尺寸连续0.25秒符合目标后准备模板，再激活游戏；自动切换失败时可在30秒内手动Alt+Tab。退出后保留竖屏。

F8 / Ctrl+C 停止采集，等待已入队的名称识别完成再导出。扫描期间保持游戏前台，不手动滚动、切换分类或排序。游戏以管理员运行时，终端也需管理员权限。

只保存当前客户区、不修改窗口或发送输入：

```powershell
.\.venv\Scripts\python.exe live_scan.py --capture-only
```

此入口需提前将窗口设为与布局相同的宽高比。窗口不唯一时用 `--list-windows` 查看，再用 `--hwnd 0x句柄` 指定。

## 离线截图

```powershell
.\.venv\Scripts\python.exe scan.py 'C:\Users\CH\Videos\SpinningMomo\截图.jpg' --category '套装'
.\.venv\Scripts\python.exe scan.py 'C:\Users\CH\Videos\SpinningMomo\截图.jpg' --category '套装' --layout-only
```

离线入口按固定网格裁切安全区域；它不会根据搜索结果自动跳过空网格。宽高比必须与布局一致，允许整张截图等比缩放。`--layout-only` 只裁图和导出，不读取字体或判定拥有状态。

两种入口均新建 `results/<时间戳>/`，也可用 `--output` 指定新目录；拒绝覆盖已有目录。已移除 `--model`、`--model-dir`、`--threads`、`--min-name-score` 等旧参数。实时入口的 `--batch-size` 仅控制进度、证据与JSONL刷新的批次，默认16件。

## 名称识别

名称裁图为 `[90,4,204,24]`，即114×20参考像素。识别流程为：

```text
参考像素裁图 → 提取暗色笔画 → 估计文字高度
  → 在邻近字号和起点中选择整行匹配最佳的字格
  → 逐字灰度向量检索 → 保存候选和核对原因
```

当前范围为9～13px整数字号，基线y=3，起点在0～6px中选择。普通13px和现场验证过的11px共用同一流程。字号估计与字符选择不接收名称标签、名称长度或名称库；模板由字体按实际字号渲染，按字号缓存，三列共用。

字体候选共8,240个字符，排除用户确认不会出现的 `•`，保留 `·`。使用完整灰度向量及矩阵乘法，不生成二进制模板。13px模板约8.8MiB；多个字号按需缓存，最多五档约38MiB，不含图像、矩阵乘法临时空间和Python本身。

`name_similarity` 为最小字符余弦相似度，`name_margin` 为最小首选/次选差距，均不是概率。当前相似度低于0.9、候选差距低于0.01、非等宽字符、空字格、超出字号范围，以及文字进入图标区域或触及边界时，保留候选并标记核对。这是保守的原型规则，尚未在整合后的完整分类上验证接受率和误识别率。

`name_icon_rect` 标出卡片右上角图标区域。该区域不参与名称高度和占位估计；与之重叠的字不能被自动认定为确定结果。被遮挡的笔画无法通过缩放恢复。非整数缩放、英文数字混排、内部空格、字体缺字和不同UI缩放尚未完整覆盖。

## 滚动与采集

每次只允许一笔未确认完成的滚动：

```text
采集新增卡片 → 发送一次输入 → 在真实新帧中确认名称锚点移动
  → 等待名称和星形前景稳定 → 确认行号接续 → 下一次输入
```

画面未响应就等待或超时停止，不追加输入。默认事务超时8秒，读取区域稳定时长0.25秒，可通过 `--frame-timeout`、`--stable-seconds` 调整。只在不同原始帧上确认稳定，旧快照不会因等待而成为新样本。

默认用6格小步标定像素/格，再计算保留重叠行的大步，后续固定使用。`--overlap-rows` 默认6；`--wheel-notches N` 可跳过标定，手动固定输入。与标定相差超过一行时停止，到底允许位移被截短。不持续学习或追加输入补偿。单次滚轮事件上限273格。

滚动锚点及局部就绪使用独立的 `name_tracking_rect: [90,4,198,24]`，不随名称裁图扩大。锚点匹配不依赖文字识别结果。窗口可以超出屏幕，鼠标滚动点选列表与显示器的可见交集，优先x=926；可用 `--scroll-point X Y` 指定参考客户区坐标。

网格先用图标底边和右边的固定亮线确认卡片存在，有卡片才检查名称是否就绪。滚动条只检测底部固定区域；确认到底后扩大卡片可读下边界、允许末行空位。网格中间空缺仍等待，避免静默漏项。

采集线程与名称识别线程并行，队列默认最多等待2屏，另有正在处理的1屏；满时暂停采集，不丢卡。WGC只保留最新帧。拥有状态取最右侧星星21个固定前景像素，独立于名称识别。

## 结果与状态

`review.html` 显示名称原图、字号、相似度、候选差距、逐字候选和核对原因。`items.json` 保存完整字符证据，CSV保存名称与主要指标。没有旧的模型输入图或OCR分数字段。

实时扫描还保存 `items.jsonl`、`session.json`、`transactions.jsonl`、`frames/` 和 `previews/`。失败时若有可用画面保存 `unmatched_frame.png`，到底保存 `end_frame.png`。

`bottom_reached` 表示滚动条确认到底，`count_verified` 还表示总数吻合；`stopped`、`scroll_limit` 保存部分结果，`failed` 表示采集或识别失败。退出码分别为0、2、1。到底与总数正确不代表名称逐字正确。

## 文件职责与验证

- `font_templates.py`：Unicode字符映射、字体灰度模板与矩阵匹配。
- `recognition.py`：字号/起点选择、逐字识别与核对标记。
- `scan.py` / `live_scan.py`：离线与实时编排。
- `stream_worker.py`：有界名称识别队列与批次证据。
- `tracking.py` / `stability.py`：名称锚点、网格接续、逐帧事务。
- `window_input.py` / `wgc_capture.py`：Win32窗口、输入与WGC。
- `layout.py` / `ownership.py` / `scrollbar.py` / `scrolling.py` / `report.py`：布局、星星判定、到底检测、滚动标定与导出。

离线核对入口及历史证据见 [实验说明](experiments/font_retrieval/README.md)。当前整合后的测试尚未运行；按约定需用户确认后运行：

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
```

测试覆盖滚动事务、末行空位、星形判定、窗口设置，以及正式名称模块的普通/小字号、列偏移、模板缓存、排除 `•` 和核对标记。单元测试不操作真实窗口或发送输入。

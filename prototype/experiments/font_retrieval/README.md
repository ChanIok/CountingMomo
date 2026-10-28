# 字体识别验证

正式识别模块已位于 `prototype/recognition.py` 与 `prototype/font_templates.py`。普通字和小字共用该实现；本目录的当前验证入口只加载它，不维护另一份识别算法。

## 当前模块的离线核对

以下命令需用户确认后运行，不捕获游戏、不移动鼠标、不发送输入。本次整合尚未运行这些验证，历史实验的准确率和耗时不能当作新模块的结果。

200条已有辅助标注的回放：

```powershell
.\.venv\Scripts\python.exe experiments\font_retrieval\character_benchmark.py
```

默认读取 `samples.json`、字体 `../temp/FZYASHJW_Zhong.ttf` 和正式布局。旧名称图为108px宽，脚本从对应的原始卡片重新裁取114px宽，避免拉伸或补造缺失文字。正确文字只用于计算指标，不传给识别器。输出到 `results/name_recognition/`，包含逐项 `review.html` 和 `report.json`，统计整名正确数、字符编辑错误、自动接受数及接受错误数。

样本中40条原为标定、160条为留出；新模块保留这些标签作探索性比较，但不再次按标签训练。它们已经用于多轮探索，不能代替新收集、独立核对的评估集。标签由助手视觉核对，尚非用户确认，修订依据保留在 `samples.json`。

用户提供的普通/长名称现场：

```powershell
.\.venv\Scripts\python.exe experiments\font_retrieval\scale_benchmark.py
```

默认输入为 `C:\Users\CH\Videos\SpinningMomo\20261024_045048_626.jpg`，按正式布局裁取两条名称，调用同一个识别器，输出到 `results/name_scale_validation/`。只确认共有前缀“于日落再相见”；末字被图标遮挡，不能计作完整名称正确样本。

两个入口都支持 `--font`、`--layout`、`--output`。第一个还支持 `--samples`，第二个支持 `--image`。不需要名称库参数。

## 保留的历史证据

以下结果属于整合之前的不同实验，不代表当前运行模块已达到相同表现：

- `results/font_retrieval_calibrated/`：名称库整行检索与重跑OCR的对照。160条评估中整行灰度160条正确，OCR146条正确。
- `results/character_retrieval/`：独立逐字实验，160条评估中灰度首选160条正确，但34条存在 `·` / `•` 同分；纯汉明129条正确，精排159条正确。保存的原始OCR为148条正确，与重跑OCR来源不同。
- `results/scale_validation/`：旧现场实验，普通13px、长名称11px；小字号模板比放大实图的前缀相似度更高。旧裁框只有108px，末字截断。
- `results/name_crop_widened/`：108px与当前114px裁图的对比。

旧的 `characters.py` 已移除，小字号实验也已改为正式模块的调用入口。历史整行对照 `benchmark.py` / `templates.py` 保留用于复现旧研究；它们不被运行入口导入，不再提供加载PaddleOCR模型的选项。已有OCR基线可用 `--paddle-report <旧report.json>` 只读复用。

## 单元测试

`tests/test_character_retrieval.py` 直接验证正式识别器，覆盖普通/小字号、起点、模板缓存、`•` 排除、背景/图标占位及核对标记。`tests/test_font_retrieval.py` 是历史二进制与评估辅助函数的用例。

本次均未运行。获得确认后可只运行名称模块：

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s tests -p test_character_retrieval.py -v
```

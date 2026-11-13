# FileWrapper 归档 `1.dat` 全功能测试报告

- 测试日期：2026-11-13
- 测试对象：`E:\study_lujiajing\WorkSpace_FileWrapper\test_files\FileProtector_MergedFile\1.dat`
- **本次只做测试与记录，未修改任何项目源码**（`git diff` 仍只有上一轮 PluginPdf 迁移的 3 个文件）
- **第二轮复核**：2026-11-13 16:45；**第三轮循环复核**：2026-11-13 17:20，见第 0 节
- **复核阶段共补跑 2 套独立验证工程 / 23 份新日志**（第二轮 6 份 + 第三轮 17 份）；全程未修改任何项目源码

---

## 0. 循环复核结论（真实性校验，第二 + 第三轮）

对第 4 节全部 **19 条**问题逐条回查源码行号与原始日志，并补跑 **两轮独立验证**：

- 第二轮：`.workbuddy/tmp/fw_verify/`，5 组独立探针；
- 第三轮：`.workbuddy/tmp/verify3/`，新建独立驱动 `fw_verify3.exe`（编译真实主程序源码、独立沙箱、独立二进制、每个 case 独立进程），共 17 个用例 = 9 个新实验 + 8 个旧用例原样重跑。

**全程没有修改任何项目源码**（`git diff` 中已跟踪文件的改动仍只有上一轮 PluginPdf 迁移的 3 个文件；新增的未跟踪文件只有本报告、`.gitignore` 与测试产物）。

**终判：19 条中 18 条确认成立、1 条剔除（P2-6）**。第二轮统计为 15 条完全确认、3 条调整定性/措辞、1 条剔除，另修正 6 处事实错误；第三轮对全部条目再核一遍，补强 6 条证据、精确化 2 条措辞、**新修正 6 处表述/事实问题**（0.2 第 7-12 条），并对被剔除的 P2-6 给出了**决定性对照实验**（窗口未 show → 37×28；show → 874×657），结论从「推断为环境伪影」升级为「实测确认为环境伪影」。

第三轮收敛判据：每条问题都必须具备 ①源码行号可复核 ②原始日志或独立复现日志 ③「现象 → 根因」因果链无断点；凡属推断的，明确标注「推断」并写明未实测的原因。第 4 节正文已按三轮结果统一更新。

### 0.1 第二轮逐条判定（保留原始轨迹，终判见 0.1b）

| 编号 | 判定 | 复核依据 / 变动 |
|---|---|---|
| P0-1 合并自覆盖 | ✅ **确认** | `DlgMergeFile::on_btnOk_clicked`（:94-108）只检查行数和路径非空，未禁止「输出 = 输入」，UI 路径同样可达；SHA/大小证据完整。仅修正一处笔误 |
| P0-2 原路径覆盖 | ✅ **确认** | SHA 三段证据齐全；独立解析确认 19 条路径全为 `G:`，且 `G:\...\FileProtector_Input` 确实不存在 |
| P1-1 畸形头部崩溃 | ✅ **确认，并补强** | 原「退出码 3」日志已被后续重跑覆盖；新建**不捕获异常**的独立探针重新实证：`EXIT=3` + `terminate called after throwing an instance of 'std::bad_alloc'` |
| P1-2 大文件全量内存 | ✅ **确认**，数值需加注 | 冷读 11.66 s / 37.4 s；连续第二次读同文件仅 **0.586 s**（OS 缓存）。「峰值 ≈2×」为源码推断，实测常驻为 1× |
| P1-3 保存重写 + 句柄敏感 | ⚠️ **部分确认，降级** | rename 失败属实，但主程序全局只有 **1 个** `FileManager`（`mainwindow.cpp:115`），第二个句柄是测试驱动人为构造，需外部程序占用才触发；磁盘峰值估算上界错误，已修正 |
| P1-4 分割 165 s 无进度 | ✅ **确认** | 660 行 qDebug、`QSplitFiles` 为 `void` 且不发任何信号 |
| P1-5 脏标记状态污染 | ✅ **确认并补强** | 新增独立证据：`unLoadMergedFile()` 后 `dirty=1`，重新 `QLoadMergedFile()` 后仍 `dirty=1` |
| P1-6 加载失败界面错乱 | ⚠️ **确认，措辞修正** | `rows=0 saveEnabled=1` 属实；但「原归档内容已无法恢复」不准确——**磁盘归档完好**，丢失的是用户已选择 Discard 的未保存改动 |
| P1-7 内容区无校验 | ✅ **确认** | 格式定义（:12-27）确无内容校验字段；SHA 实证损坏数据被导出 |
| P2-1 同名覆盖 | ✅ **确认** | 日志 `output files=1`，幸存内容为 B |
| P2-2 插件映射未实现 | ✅ **确认** | `:289` 提示映射、`:1216` 「功能开发中」 |
| P2-3 校验入口不一致 | ✅ **确认** | `validateMergedFile` 只调 `readAndValidateHeader`；分割入口 `:859` 依赖它 |
| P2-4 进度文案 | ⚠️ **确认，降级** | 进度条 `progressChanged(copiedTotal, totalBytes)` **计算正确**；问题只是状态文字显示「当前文件进度」，口径不一致，非计算错误 |
| P2-5 失败信息笼统 | ✅ **确认** | `:1196-1200` 固定文案 |
| P2-6 图片 37×28 | ❌ **剔除（测试环境伪影）** | 独立 Qt 探针：未 `show()` 的 `QScrollArea` 的 `viewport()->height()` = **28**，`show()` 后 = **698**。offscreen 下插件窗口从未 show，必然算出 37×28。**真实 GUI 不会出现**（当时为推断，第三轮 show 对照已实测坐实）；代码缺 `resizeEvent` 仅保留为低优先级改进项 |
| P2-7 EPUB 标题 | ✅ **确认，根因精确化** | 第二轮用 Python 复现 Qt 正则（第三轮已改用真实 `QRegularExpression`，见 0.1b）：OPF 里是 `<dc:title>从一到无穷大</dc:title>`，而 `textOfTag`（`PluginBook/mainwindow.cpp:577-586`）的闭合模式 `(?:<|:)/title` **匹配不了 `</dc:title>`** → 返回空 → 回退「未命名电子书」。首屏空白确属文档本身 |
| P2-8 绝对路径 | ✅ **确认** | 独立解析 19/19 为 `G:` |
| P3-1 `int` 截断 | ✅ **推断成立** | 三处源码行号、三个换算值复核无误 |
| P3-2 其它健壮性 | ✅ **推断成立** | 各条目行号复核无误 |

### 0.1b 第三轮终判（逐条独立复现 / 再核验）

| 编号 | 终判 | 第三轮动作与结果 |
|---|---|---|
| P0-1 | ✅ 确认 | 重跑 `merge`：4096 B → 137 B，源 SHA 改变；产物仍只有头。新沙箱下 `afterSha=37ed8dec…`（与首轮 `64309427…` 不同，因头部记录了输入文件的绝对路径） |
| P0-2 | ✅ 确认 | 重跑 `oldpath`：alpha/beta 三段 SHA 与首轮**逐字节一致** |
| P1-1 | ✅ 确认 | 新建独立进程探针 `v3/nocatch`（该调用链上**无任何 try/catch**）：`validate=1 loaded=1 firstSize=2^63-1` → `terminate … std::bad_alloc`，**EXIT=3** |
| P1-2 | ✅ 确认 | 源码链逐行复核（`:1116` new / `:1163` 一次 write / 插件 `:1063` av_malloc）+ 冷读 11.66 s、37.4 s 日志复核；「瞬时峰值 2×」保持为**源码推断**，未直接测量 |
| P1-3 | ✅ 确认（降级结论不变） | 重跑 `save`：`2nd save ok=0 … dirty=1`；确认主程序全局仅 1 个 `FileManager`（`:115`） |
| P1-4 | ✅ 确认 | 精确统计 `run_split_full.log`：676 行中 **660 行**为 `qDebug split`，`elapsed=165096ms`，与报告一致 |
| P1-5 | ✅ 确认，**措辞修正** | 新实验 `dirtyui`：加载新归档 B 后 `saveEnabled=0`，但随后点「卸载」**仍弹「未保存的改动」确认框** → 实证 dirty 未重置；原报告「点保存会重写」不成立（按钮此时是灰的），已改为「在确认框中选保存才会全量重写」 |
| P1-6 | ✅ 确认，**根因精确化** | 重跑 `add_del`：`rows=0 saveEnabled=1`；查明失败路径**确实调用了** `updateViewActionState()`（`:644`），但该函数只管视图切换按钮、不刷新 `m_acSaveFile`；`save()` 因 `isArchiveLoaded()==false` 直接返回 false |
| P1-7 | ✅ 确认 | 重跑 `corrupt`：`sha before=5945dc7e… after=9466b377… equal=0`，导出无提示 |
| P2-1 | ✅ 确认 | 重跑 `dupname`：`surviving content='BBBB-FROM-DUP-B'`，输出文件数 1 |
| P2-2 | ✅ 确认 | 源码 `mainwindow.cpp:289` / `:1216` 与 `run_open_big_3.log` / `run_misc.log` 复核一致 |
| P2-3 | ✅ 确认，**推断升为实测** | 新实验 `splitbad`：`validate=1` → `QSplitFiles` 正常返回、`outputFiles=0`、上层无任何信号/反馈 |
| P2-4 | ✅ 确认，**数值修正** | 重跑 `merge`：`lastStatus='正在合并: __add_probe.txt  (27 B / 27 B)'`；修正「总数据量 14,320 B」→ 内容总量 **13,848 B**，14,320 B 是含 472 B 头部的归档总大小 |
| P2-5 | ✅ 确认 | `:1196-1200` 固定文案、`QSplitFiles` 全程仅 qWarning 且返回 void，复核一致 |
| P2-6 | ❌ **剔除（决定性证据）** | 同一驱动、同一双击路径：`ui_hide` → `pixmap=37x28`；`ui_show` → `pixmap=874x657`。插件级对照：viewportH 28 → 37×28，viewportH 649 → 864×649。**确认是 offscreen 未 show 的伪影** |
| P2-7 | ✅ 确认，**证据升级** | 不再依赖 Python 模拟：用真实 `QRegularExpression` 跑 `textOfTag` 原正则 → `hasMatch=0`；namespace-aware 正则 → `captured='从一到无穷大'` |
| P2-8 | ✅ 确认 | 独立 Python 重新解析 `1.dat`：19/19 路径以 `G:/` 开头，最大偏移 `10,846,329,369` |
| P3-1 | ✅ 推断成立 | C++ 实测 `(int)qint64`：`4907991040→613023744`、`4125556736→-169410560`、`1711670246→1711670246`，与报告三值完全一致 |
| P3-2 | ✅ 推断成立 | 6 条子项行号逐条复核无误 |

### 0.2 事实 / 单位 / 措辞错误修正清单（三轮累计 12 处）

| # | 原报告 | 修正为 | 依据 |
|---|---|---|---|
| 1 | 「6 个内容偏移 > 2^32」 | **15 个** | 独立 Python 解析 19 条目 |
| 2 | 多处「10.85 GiB」 | **10.85 GB（十进制）= 10.10 GiB** | `10846329463 / 2^30 = 10.1014` |
| 3 | 保存磁盘峰值「21.7~32.6 GB」 | **约 21.7 GB（2×）** | `rename` 只改目录项不复制数据，`.bak.fwda` 不额外占空间 |
| 4 | P0-1「因 `copied < 0` 不成立」 | 因 `copied < fileVec[i].size`（此时 size=0）不成立 | `filemanager.cpp:402` |
| 5 | P1-1 引用「run_invalid.log 首轮日志」 | 引用 `fw_verify/run_nocatch.log` | 原日志已被覆盖；且当前驱动 `try/catch` 了异常，其 `PASS 未崩溃` **不能**证明程序不崩溃 |
| 6 | P2-6「全项目搜索无 resizeEvent」 | **PluginPicture 没有**；PluginMedia 有（`:614`） | 全项目 grep |
| 7 | P1-5「加载新归档后点保存会全量重写」 | **不成立**：加载成功后 `mainwindow.cpp:657` 强制把保存按钮置灰，用户点不了；只有在后续 `confirmDiscardOrSaveIfDirty()` 的确认框里选「保存」才会触发全量重写 | `v3_v3_dirtyui.log` + 源码 `:657` |
| 8 | P2-4「本次合并总数据量是 14,320 B」 | 内容总量 **13,848 B**（1870+94+11857+27）；14,320 B 是含 472 B 头部的**归档总大小** | `v3_merge.log` 头部 `hdrSize=472 size=14320` |
| 9 | 第 6 节「对 8 类畸形输入均正确拒绝」 | **7 类**（截断 / 错 magic / 错 CRC / count=0 / count 巨大 / hdrSize 巨大 / version=2），括号里也正好列了 7 项 | `run_invalid.log` 中 `validate=0` 的用例数 |
| 10 | P1-6「没有刷新保存按钮」 | 措辞不准：失败路径**确实调用了** `updateViewActionState()`（`:644`），但该函数（`:995-1004`）只管 4 个视图切换按钮，**不包含** `m_acSaveFile` | 源码逐行复核 |
| 11 | P0-1 引用的 `afterSha=64309427…` | 该值**与沙箱绝对路径相关**（头部记录了输入路径），第三轮独立沙箱为 `37ed8dec…`；核心证据是 `4096 B → 137 B` 且 SHA 改变，与具体摘要值无关 | `run_merge.log` vs `v3_merge.log` |
| 12 | P1-6 正文「修改建议」段落重复出现两次 | 删除重复的一行 | 编辑校对 |

### 0.3 第二轮新增的独立证据

| 文件 | 结果 |
|---|---|
| `fw_verify/run_nocatch.log` | 不捕获异常：`validate=1 loaded=1 entries=4 firstSize=2^63-1` → `terminate ... std::bad_alloc`，**EXIT=3** |
| `fw_verify/run_control.log` | 同一归档但调用方 catch：EXIT=0，证明崩溃确由该异常引起 |
| `fw_verify/run_state.log` | `after add dirty=1`；`after unLoadMergedFile dirty=1`；`after QLoadMergedFile reload dirty=1` |
| `fw_verify/run_viewport.log` | 未 show 的 viewport 高 = 28，show 后 = 698 → 证明 37×28 是环境伪影 |
| `fw_verify/run_bigread5_2nd.log` / `_3rd.log` | 冷读 12041 ms，紧接的第二次 586 ms → 覆盖矩阵 #6 的「1.0 s」是缓存命中，不是性能基线 |

### 0.3b 第三轮新增独立证据（`.workbuddy/tmp/verify3/logs/`）

| 文件 | 结果 |
|---|---|
| `v3_v3_ui_hide.log` / `v3_v3_ui_show.log` | 同一 `MainWindow` + 同一双击路径：未 show → `pixmap=37x28`；`w.show()` → `pixmap=874x657` |
| `v3_v3_plugin_hide.log` / `v3_v3_plugin_show.log` | 插件级对照：viewportH=28（未 show）/ 649（show），pixmap 37×28 / 864×649，原图 4624×3472 |
| `v3_v3_regex.log` | 真实 `QRegularExpression`：`textOfTag` 原正则 `hasMatch=0`；namespace-aware 正则 `captured='从一到无穷大'` |
| `v3_v3_splitbad.log` | `validate=1` → `QSplitFiles` 正常返回、`outputFiles=0`、无任何信号 |
| `v3_v3_dirtyui.log` | 加载新归档 B 后 `saveEnabled=0`，但随后「卸载」仍弹「未保存的改动」确认框 → dirty 未重置 |
| `v3_v3_nocatch.log` | 该调用链无 try/catch 的独立进程：`EXIT=3` + `terminate called after throwing an instance of 'std::bad_alloc'` |
| `v3_v3_intcast.log` | 三个 `qint64 → int32` 实测换算值，与报告完全一致 |
| `v3_merge / oldpath / corrupt / dupname / save / invalid / add_del / open_all.log` | 8 个旧用例在**独立二进制 + 独立沙箱**中原样重跑，结论与首轮/二轮全部一致 |
| `verify3/make_fixtures.py` 输出 | 独立复算 `1.dat`：CRC `0x6514C0E9` 匹配、19 条目、15 个偏移 > 2^32、19/19 路径 `G:`、10.1014 GiB = 10.8463 GB |
| `verify3/refs_dump.txt` / `log_refs.txt` | 报告全部源码行号引用与日志引用的自动核对结果（21 个引用日志文件全部存在） |

> **第三轮终判**：18 条确认成立、1 条剔除；三轮累计 3 条调整定性、6 条补强证据，**P2-6 剔除**已由决定性对照实验坐实，不再是推断。所有引用行号、日志片段、数值均经自动脚本 + 人工双重复核，本轮**未发现新的误报**，另修正 6 处表述/事实问题（0.2 第 7-12 条）。仍标注为「推断」的只剩：P1-2 的瞬时内存峰值、P1-3 / P0-2 对 10.85 GB 归档的磁盘与耗时外推、P3-1 / P3-2 的静态风险，均已在正文写明未实测原因。第 4 节正文已按三轮结果统一更新。

---

## 1. 测试对象与归档解析

用独立 Python 解析头部（不依赖被测程序），与程序内 `getFileInfos()` 结果逐项比对：

| 项 | 值 |
|---|---|
| 文件大小 | 10,846,329,463 B（10.101 GiB） |
| Magic / Version | `FWDA` / 1 |
| HeaderSize / FileCount | 2130 B / 19 |
| HeaderChecksum | `0x6514C0E9`（CRC32 校验通过） |
| 内容区 | 10,846,327,333 B = 19 个条目大小之和 |
| 校验 | `2130 + Σ(fileSize) = 文件大小`，完全吻合 |

19 个条目中：

- 3 个超过 1 GiB：`Windows10.iso` 4,907,991,040 B、`vs2015.pro_chs.iso` 4,125,556,736 B、`large_input.mp4` 1,711,670,246 B
- **15 个**内容偏移 > 2^32（最大 `10,846,329,369`）——程序用 `quint64/qint64` 处理，**未发现 4GB 偏移溢出问题**（第二轮独立解析修正：首轮写成 6 个）
- 条目路径全部是 `G:/study_lujiajing/WorkSpace_FileWrapper/test_files/FileProtector_Input/...`，而项目现在位于 `E:`，且 G 盘上该目录**已不存在**（G 盘本身存在，剩余 343 GB）

> 单位说明：文件大小 10,846,329,463 B = **10.101 GiB = 10.846 GB（十进制）**。下文出现的「10.85 GB」均为十进制口径，与表格中的 10.101 GiB 是同一个数。

## 2. 测试环境与方法

| 项 | 值 |
|---|---|
| 工具链 | Qt 6.9.3 / MinGW 13.1.0（`E:\V_C\Qt`），Windows，offscreen 平台 |
| 物理内存 | 测试期间可用约 5.4~6.3 GB |
| 磁盘 | E: 剩余约 250 GB |
| 真实程序 | `build\Desktop_Qt_6_9_3_MinGW_64_bit_Debug\FileWrapper\debug\FileWrapper.exe` 实际启动成功，5 个插件全部 `[Plugin] Loaded`（含 PluginPdf） |
| 自动化测试驱动 | `.workbuddy/tmp/fw_test/fw_test.pro` + `test_main.cpp`：**直接编译主程序全部源码（除 main.cpp）**，真实实例化 `MainWindow` / `FileManager` / 全部 5 个插件 / 全部对话框，通过元对象调用真实槽函数、自动应答模态对话框、模拟 `treeView->doubleClicked` |
| 隔离方式 | 每个 case 独立进程，崩溃/超时不影响其它用例；首轮 26 份运行日志，第二轮 6 份独立日志（`fw_verify/run_*.log`），第三轮 17 份（`verify3/logs/v3_*.log`） |

> 说明：这是 offscreen 自动化集成测试，不是 GUI 手工点按验收。涉及窗口尺寸/视觉的结论已在对应条目中标注需人工复核。

## 3. 功能覆盖矩阵

| # | 功能 | 用例 | 结果 |
|---|---|---|---|
| 1 | 启动 + 插件加载 | `smoke`、真实 exe 冒烟 | ✅ 5/5 插件加载成功 |
| 2 | 加载归档 / 头部校验 | `load` | ✅ `validateMergedFile=true`，19 条目的大小/偏移/路径与独立解析一致 |
| 3 | 树形 / 平铺 / 显示路径 / 显示名称 | `views`（6 次切换） | ✅ 每次切换叶子数恒为 19，按钮 enable 状态正确 |
| 4 | 双击打开（16 个 ≤100MB 文件） | `open_all` | ✅ 全部打开且插件实例正常；图片 37×28 已由第三轮 `show()` 对照实验坐实为 offscreen 伪影（show 后 874×657，见 P2-6）、EPUB 首屏空白属文档本身（见 P2-7） |
| 5 | 打开未知后缀（ISO） | `open_big 3` | ⚠️ 弹「没有插件来处理该类型文件，请手动映射插件」，但映射功能未实现 |
| 6 | 打开 1.59 GiB MP4 | `open_big 5` | ✅ 时长 103:57、首帧渲染，常驻约 1.6 GB 内存；「1.0 s」是**文件缓存命中**（复核实测冷读 11.66 s / 热读 0.59 s），不能作为性能基线 |
| 7 | 多标签页：新建/打开/切换/关闭 | `tabs` | ✅ 独立实例、切换保留内容、关闭正常 |
| 8 | 添加 / 删除 / 同路径去重 | `add_del` | ✅ 19→20→19，重复添加被去重，保存按钮状态正确 |
| 9 | 保存（小归档增删后重写） | `save` | ⚠️ 首次成功且内容完整；第二次在存在另一只读句柄时失败 |
| 10 | 导出（94 B / 1.8 KB / 5 MB） | `export 18/7/0` | ✅ 大小 + SHA256 与归档内原始数据、磁盘源文件三方一致 |
| 11 | 导出 1.59 GiB / 4.57 GiB | `export 5/3` | ⚠️ 结果正确，但耗时 13.4 s / 71.8 s，全程阻塞无进度 |
| 12 | 合并（含中文名、4 文件） | `merge` | ✅ 头部 CRC 有效，4 个条目内容 SHA256 与源文件一致 |
| 13 | 合并 UI 流程 | `merge_ui` | ✅ 对话框→进度→「文件合并成功」→归档 2213 B |
| 14 | 分割（小归档） | `split small`、`split_ui` | ✅ 全部条目大小 + SHA256 一致 |
| 15 | **分割 `1.dat` 全量 10.85 GB** | `split full` | ✅ 19/19 全部匹配，耗时 **165.1 s**；⚠️ 无进度/取消/完成提示 |
| 16 | 按原路径分割 | `oldpath` | ⚠️ 静默覆盖磁盘上已被修改的源文件 |
| 17 | 卸载 / 关于 / 插件映射 | `misc` | ✅ 卸载清空列表；⚠️ 插件映射仅提示「功能开发中」 |
| 18 | 异常归档输入（10 种） | `invalid` | ⚠️ 多数被正确拒绝；但伪造 `fileSize` 可导致 `std::bad_alloc` 终止进程 |
| 19 | 内容区篡改检测 | `corrupt` | ❌ 校验通过、损坏数据被静默导出 |
| 20 | 同名不同目录分割 | `dupname` | ❌ 2 个条目输出成 1 个文件，后者覆盖前者 |

---

## 4. 问题清单与修改建议

> 每条都标注了证据类型：【实测】= 本次跑出日志/数值；【推断】= 源码审查 + 确定的语言/平台语义，未在本次样本中直接触发。
> 代码位置均为 `E:\study_lujiajing\WorkSpace_FileWrapper\FileWrapper\` 下的相对路径与行号。

### P0 — 数据损坏级，建议立即修复

#### P0-1 合并时「输出路径 = 某个输入文件」会静默清空该源文件 【实测】

- **复现**：`QMergeFiles([victim.bin], victim.bin)`，`victim.bin` 原为 4096 B。
- **证据**：`.workbuddy/tmp/fw_test/run_merge.log`
  `self-overwrite merge: beforeSize=4096 beforeSha=7efbe488eeb8ed87 afterSize=137 afterSha=64309427c6cab795`；
  产出的归档 `hdrSize=137 count=1 size=137`（**只有头，内容为空**），`fileVec[0].size` 被读成 0。
  第三轮在独立沙箱原样重跑（`verify3/logs/v3_merge.log`）得到 `4096 → 137`、`afterSha=37ed8dec…`，结论一致。
  ⚠️ 头部记录了输入文件的**绝对路径**，不同沙箱下产物 CRC/SHA 必然不同（`0x50356845` → `0x470126df`）；判定依据是「源文件被截成 137 B、只剩一个空条目」，不是某个固定摘要值。
- **根因**：`FileWrapper/filemanager.cpp:267-273` 先 `fOut.open(QIODevice::WriteOnly)` 把输出文件截断；`:289-303` 之后才打开输入文件读 `size()`，此时已是 0。写内容循环 `:402` 的条件 `copied < fileVec[i].size` 变成 `0 < 0`，不成立，因此一个字节都不写。
- **UI 可达性（复核补证）**：`DlgMergeFile::on_btnOk_clicked`（`FileWrapper/dlgmergefile.cpp:94-108`）只校验「待合并列表非空」和「输出路径非空」，不检查输出路径是否与输入重复；`duplicatCheck()`（`:115-125`）也只去重输入列表内部。输出路径由 `on_btnMergedFilePath_clicked`（`:82-92`）用 `QFileDialog::getSaveFileName` 选取，Qt 自带的「文件已存在，是否替换？」只确认覆盖，**不会提示「该文件同时是输入，会被先截断再读取」**，所以这不是只有直接调 API 才能触发的人造场景。
- **修改建议**：
  1. 合并前用 `QFileInfo::canonicalFilePath()` 逐一比较输出路径与所有输入路径（也要展开目录递归后的文件），命中即拒绝并提示「输出文件不能是待合并文件之一」；
  2. 检查输出文件是否已存在并提示覆盖确认；
  3. 更稳妥：始终先写 `xxx.tmp.fwda`，全部成功且校验通过后再原子替换，避免任何中途失败留下半截归档；
  4. 预检磁盘可用空间 ≥ Σ 输入大小 + 头部。

#### P0-2 「按原路径分割」静默覆盖磁盘上已修改的源文件 【实测】

- **复现**：小归档里记录两个绝对路径，合并后故意修改原文件，再执行 `QSplitFiles(arc, true, "")`。
- **证据**：`run_oldpath.log`
  `alpha before=b36edc458f18 modified=c0f00feba078 after=b36edc458f18`（beta 同理）——用户合并之后的编辑被无声抹掉。
- **对本 `1.dat` 的具体风险（推断，未实测）**：19 条路径全是 `G:/...`（第三轮独立解析再次确认 19/19），`QDir("G:/").exists()=true` 但原目录已不存在。按源码 `:552-566`，若用户勾选「按原路径分割」，程序会 `mkpath` 在 G 盘新建目录并写入 **10.85 GB**；换一台机器或换盘符后，这些绝对路径也可能指向完全无关甚至系统目录。**未实际执行这一步**，以免在用户磁盘上写入 10.85 GB；依据是小归档上的 `oldpath` 实测 + 上述源码路径。
- **根因**：`filemanager.cpp:552-555` 直接采用归档内记录的绝对 `origPath`；`:558-566` 自动 `mkpath`；`:575` `open(QIODevice::WriteOnly)` 直接覆盖，无存在性检查、无确认、无备份。
- **修改建议**：
  1. 默认改为「输出到用户选定目录」，把「按原路径」改成需二次确认的高级选项；
  2. 覆盖前检测目标文件存在且内容不同 → 弹出「覆盖 / 跳过 / 重命名 / 全部应用」；
  3. 归档格式改为记录**相对路径**（+ 可选原始根路径），避免跨机器/盘符迁移后写到错误位置；
  4. 对路径做安全校验：拒绝绝对路径、`..` 回溯、Windows 非法字符与设备名。

### P1 — 高优先级

#### P1-1 头部不校验条目字段，伪造 `fileSize` 可让进程崩溃 【实测】

- **复现**：取一个合法归档，重算 CRC 后把第 0 个条目的 `FileSize` 改成 `0x7FFFFFFFFFFFFFFF`。
- **证据（多轮实证）**：`run_invalid.log` 中
  `filesize_huge | validate=1 loaded=1 entries=4 firstSize=9223372036854775807 firstPos=472`。
  ⚠️ **该文件当前版本的测试驱动用 `try/catch` 接住了异常，末尾的 `PASS …未崩溃` 不能作为「程序不会崩」的证据**；真实调用方 `MainWindow::renderFile`（`mainwindow.cpp:451-464`）没有任何异常保护。
  因此补跑了**不捕获异常**的独立探针 `.workbuddy/tmp/fw_verify/run_nocatch.log`：
  `VERIFY BEFORE QGetFileContent` → `terminate called after throwing an instance of 'std::bad_alloc'` → **进程 EXIT=3**；
  对照 `run_control.log`（仅调用方加 catch）EXIT=0，确认崩溃确由该异常引起。
  第三轮再用独立编译的 `fw_verify3.exe` 的 `v3/nocatch` 用例复现一次（该调用链无任何 try/catch，且每个 case 独立进程；其它用例中的 catch 不在本进程的执行路径上）：`verify3/logs/v3_v3_nocatch.log` → `validate=1 loaded=1 firstSize=9223372036854775807` → `terminate called after throwing an instance of 'std::bad_alloc'` → **EXIT=3**。主程序 `FileWrapper/` 目录下 grep `try/catch` **零命中**，崩溃路径确凿。
  另有 `contentpos_huge`：`firstPos=9223372036854775807` 也被接受，读取时仅打印一条 `qWarning 读取文件内容不完整`，返回 `buf=null len=0`，**UI 层无任何提示**。
- **根因**：`filemanager.cpp:134-230` 只校验 magic / version / count / hdrSize / CRC；`:639-706` 解析条目时不校验 `fileSize`、`contentPos` 与文件实际大小的关系、区间是否重叠；`:1116` 无上限地 `new char[nFileLen]`，且无异常保护。
- **修改建议**：
  1. 加载时逐条目校验：`0 <= fileSize`、`contentPos >= hdrSize`、`contentPos + fileSize <= 文件实际大小`、各条目区间不重叠、`pathLen` 合理、路径 UTF-8 合法且不含危险字符；任一项失败即整档判为无效并给出原因；
  2. `new (std::nothrow)` 或 `try/catch (std::bad_alloc)`，失败时返回可显示的错误而不是终止进程；
  3. 把「头部 CRC 通过」与「归档有效」解耦，另见 P2-3。

#### P1-2 单文件全量读入内存：大文件长时间阻塞、峰值内存可达 2× 文件 【实测 + 推断】

| 操作 | 文件 | 实测 | 内存变化 |
|---|---|---|---|
| `QGetFileContent`（冷读） | `large_input.mp4` 1.59 GiB | 11.66 s | 5604 → 3969 MB |
| `QGetFileContent`（冷读） | `Windows10.iso` 4.57 GiB | **37.4 s** | 5609 → **1701 MB** |
| 导出（含写盘） | `large_input.mp4` | 13.4 s | — |
| 导出（含写盘） | `Windows10.iso` | **71.8 s** | — |
| 打开（缓存命中） | `large_input.mp4` | 1.0 s | 5602 → 3949 MB |

- **证据**：`run_bigread_5.log`、`run_bigread_3.log`、`run_export_5.log`、`run_export_3.log`、`run_open_big_5.log`。
- **复核补充（耗时口径）**：第二轮连续重跑同一 1.59 GiB 条目，冷读 **12041 ms**、紧接着的第二次仅 **586 ms**（`fw_verify/run_bigread5_2nd.log` / `_3rd.log`）。所以「打开只花 1.0 s」是 OS 文件缓存命中，**不能用来证明大文件读取没有性能问题**；可靠的基线是冷读 11.66 s / 37.4 s，以及含写盘的导出 13.4 s / 71.8 s。
- **根因**：`filemanager.cpp:1069-1126` 一次性 `new char[nFileLen]` + 一次 `QFile::read`；`mainwindow.cpp:451-464` `renderFile` 把整块送给插件；`:1133-1167` `outputFile` 同样全量读；`Plugins/PluginMedia/mainwindow.cpp:1063-1070` 又 `av_malloc(nFileLen)` + `memcpy` 再复制一份（已复核源码确在）→ **瞬时峰值约 2 倍**，`renderFile` 在 `sendFileData` 返回后 `delete[]` 自己的缓冲，所以日志里看到的**常驻**只有 1×，两者不矛盾。本机可用内存仅 5.5 GB，4.57 GiB 条目已把可用内存压到 1.7 GB，若这类文件被路由到媒体插件，第二次分配极可能失败。
- **修改建议**：
  1. 把 `QGetFileContent(char**, qint64)` 换成流式接口：给插件一个 `QIODevice*`（归档区间只读设备）或 `read(offset,len)` 回调，插件按需读取；
  2. 或在大文件（建议阈值 64~128 MB）时把内容抽取到临时文件，把**文件路径**传给插件（FFmpeg 可直接 `avformat_open_input` 文件，QtPdf 支持 `load(path)`，图片/文本可 QFile 读取）；
  3. 保留全量路径时至少加：大小上限、`QProgressDialog` + 取消、`std::nothrow` 失败提示；
  4. 所有读写改为分块循环，不要一次 `write(buf, nFileLen)`。

#### P1-3 保存是全量重写；文件被外部占用时 rename 失败且原因不可见 【实测 + 推断；全量重写部分仍列 P1，句柄占用子项经复核降为 P2】

- **复核后的定性**：「全量重写 10.85 GB」是真实且重要的问题；但「第二个只读句柄」是**测试驱动自己创建的第二个 `FileManager`**，而主程序全局只有 **1 个** `FileManager`（`mainwindow.cpp:115` `m_fileManager = new FileManager`）。因此该失败只在**外部程序（另一个 FileWrapper 实例、资源管理器预览、杀毒/索引服务）打开同一 `.dat`** 时才触发，属于 Windows 文件共享模式下的健壮性问题，不是单实例常态 bug。另外 `save()` 失败时保持 `m_bDirty=true` 是**正确设计**（改动仍在内存中），原标题「失败后状态残留」措辞不准，已去掉。
- **实测**：`run_save.log` —— 同一进程内另一个 `FileManager` 持有归档只读句柄时，
  `[qwarn] 备份原文件失败，取消替换`，`2nd save ok=0 ... dirty=1`；调用 `unLoadMergedFile()` 关闭该句柄后，`3rd save ok=1 dirty=0`。
- **根因**：`filemanager.cpp:1034-1061` 关闭自己的句柄后用 `QFile::rename(sArchivePath, sBak)`；Windows 下只要有其它进程/句柄以无 `FILE_SHARE_DELETE` 方式打开该文件，rename 就失败（代码 `:1032-1033` 的注释也承认这一点）。失败分支只 `qWarning`，UI 只显示笼统的「保存失败，归档文件可能已损坏…」（`mainwindow.cpp:1196-1200`），用户无法知道真实原因。
- **对 `1.dat` 的推断**（未实际执行，避免改动用户数据）：`rewriteArchive()` 会流式重写全部 10.85 GB；`QFile::rename` 只改目录项、不复制数据，`.bak.fwda` 不额外占用空间，因此磁盘峰值 ≈ 原归档 + `.tmp.fwda` ≈ **21.7 GB（2×）**（首轮写成 21.7~32.6 GB，已修正）；按 `split full` 实测 10.85 GB 读+写耗时 165 s 外推，即使只删掉那个 94 B 的 `记录排重RepeatRecord.txt`，也要重读重写 10.85 GB、耗时数分钟，且期间 UI 冻结。
- **修改建议**：
  1. 改为增量格式：追加新文件 + 逻辑删除标记，定期 compact；或至少对「只删除/只追加」做快速路径；
  2. 替换阶段用 Windows `ReplaceFileW` / `MoveFileEx(MOVEFILE_REPLACE_EXISTING)`，并确保所有相关句柄先关闭；失败时清理 `.bak.fwda` 并把具体原因（磁盘空间不足 / 文件被占用 / 只读）回传 UI；
  3. 重写前检查可用磁盘空间，不足则直接拒绝并说明需要多少；
  4. 保存失败时保持 `m_bDirty` 并明确提示「改动仍在内存中，可重试」，不要让人误以为已丢失或已保存。

#### P1-4 分割 10.85 GB 耗时 165 s，无进度 / 无取消 / 无完成提示，UI 完全冻结 【实测】

- **证据**：`run_split_full.log` `QSplitFiles elapsed=165096ms`、`output file count=19`、`split verify match=19 mismatch=0 missing=0`；日志共 676 行，其中 **660 行**是 `qDebug() << "split" ...`（每 16 MB 一条，第三轮用脚本按 `\[debug\] split` 精确计数复核为 660/676）。
- **根因**：`mainwindow.cpp:852-874` 直接 `emit QSplitFiles(...)`，信号直连到主线程槽，没有进度对话框；`filemanager.cpp:469-605` 既不 emit `progressChanged` / `operationFinished`，也不检查 `m_bCancelRequested`（与 `QMergeFiles` / `rewriteArchive` 不一致）；`:599` 在热循环里 qDebug 刷屏。
- **修改建议**：把分割搬到工作线程（与合并一致用 `QThread` + 信号），复用 `QProgressDialog` + `cancelOperation` + `operationFinished`，完成后弹出「共 N 个文件 / 跳过 M 个 / 失败 K 个」；移除热循环 qDebug 或改为 `Q_ELAPSED_TIMER` 节流。

#### P1-5 加载/卸载不重置脏标记与待添加集合，跨归档状态污染 【实测】

- **证据**：`run_save.log` —— `fm3 dirty before reload=1`，随后 `QLoadMergedFile(另一个归档)` 得到 `dirty AFTER=1 entries=4`（应为 false）。
  第二轮补充独立探针 `fw_verify/run_state.log`：`after add dirty=1 entries=5` → `after unLoadMergedFile(): dirty=1 loaded=0 entries=0` → `after QLoadMergedFile reload: dirty=1 entries=4`。**加载与卸载两条路径都不重置脏标记**，已实证。
  第三轮补做 **UI 级**实证 `verify3/logs/v3_v3_dirtyui.log`：`after load A: saveEnabled=0` → `after ADD: saveEnabled=1` → `after load B: saveEnabled=0`（内部 dirty 仍为 1）→ 随后点「卸载」**再次弹出「当前归档存在未保存的改动…是否先保存？」**。
- **根因**：`filemanager.cpp:609-613`（`QLoadMergedFile`）与 `:709-715`（`unLoadMergedFile`）只 `clear()` 了 `m_vtFileInfos` / `m_mapFileInfos`，没有重置 `m_bDirty`、`m_setPendingFiles`、`m_bCancelRequested`。可观察后果有三点：
  1. **界面与内部状态矛盾**：`mainwindow.cpp:657` 在加载成功后硬编码 `m_acSaveFile->setEnabled(false)`，但 `m_bDirty` 仍为 1 —— 按钮显示「无需保存」，内部却认为「有未保存改动」；
  2. 下一次执行会调用 `confirmDiscardOrSaveIfDirty()` 的操作（加载 `:630`、卸载 `:1221`）时**误弹**未保存确认框（第三轮已实证）；
  3. 若在该确认框中选「保存」，`confirmDiscardOrSaveIfDirty()`（`:612-615`）会调 `save()`，对刚加载、根本没改过的新归档执行 `rewriteArchive()` 全量重写。
  ⚠️ 二轮报告写的「此时点保存会把新归档全量重写一遍」**不准确**——按钮此时是灰的，普通点击不可用；已按上述修正。首轮报告写的「可能把上一个归档的待添加文件写进新归档」经复核**不成立**：`rewriteArchive()` 只遍历当前 `m_vtFileInfos`，旧 `m_setPendingFiles` 里的路径不在其中就不会被读取，已删除该表述。
- **修改建议**：两个函数开头统一 `m_bDirty=false; m_setPendingFiles.clear(); m_bCancelRequested=false; m_qFile.close()`；把「解析到临时结构 → 成功后原子替换」作为唯一的状态变更点。

#### P1-6 加载无效归档后界面与内部状态不一致，当前归档被卸载 【实测】

- **证据**：`run_add_del.log` —— 已有归档且有未保存改动时加载一个非归档文件，弹两次提示后 `after 2nd load rows=0 saveEnabled=1`。列表空了，但「保存文件」按钮仍可用；此时点保存会因 `isArchiveLoaded()==false` 直接失败。
- **复核修正**：首轮写的「原归档内容已无法恢复」**不准确**——磁盘上的 `.dat` 完好无损，重新加载即可恢复；丢失的只是内存中未保存的改动，而这还是用户在 `confirmDiscardOrSaveIfDirty()` 里主动选了「Discard」的结果。真正的缺陷是：**一次失败的加载把原本有效的归档也一起卸载了**（列表清空、句柄关闭），且工具栏状态没有完整同步（保存按钮未被刷新），界面停在「空列表 + 保存按钮可用」的矛盾状态。
- **根因**：`mainwindow.cpp:637-648` 先无条件调用 `QLoadMergedFile()`（`filemanager.cpp:609-629` 内部已清空条目并关闭句柄），失败后才清空视图并弹错误框，没有回滚、没有恢复原归档句柄。失败分支 `:644` **确实调用了** `updateViewActionState()`，但该函数（`:995-1004`）只刷新 4 个视图切换按钮，**不刷新 `m_acSaveFile`**；成功路径 `:657` 才会把保存按钮置灰，失败路径缺这一步，于是按钮停留在操作前的状态（日志 `saveEnabled=1`）。另外 `isArchiveLoaded()`（`:717-720`）基于 `m_qFile.isOpen()`，句柄已关，`save()`（`:790`）会直接返回 false，用户点这个「可用」的保存按钮只会失败。
- **修改建议**：加载前先 `validateMergedFile()` + 完整解析到临时对象，成功再替换当前归档；失败时保持原归档、原视图、原脏状态不变，只弹错误提示；把工具栏状态刷新（含保存按钮）抽成单一函数并在这条失败路径也调用。

#### P1-7 归档内容区无任何校验，损坏数据被静默导出 【实测】

- **证据**：`run_corrupt.log` —— 把第一个条目内容区第 10 字节翻转后：
  `validateMergedFile=1 loaded=1 entries=4`，导出文件 SHA256 `9466b377…` 与源文件 `5945dc7e…` 不同，**全程无任何提示**。第三轮独立重跑（`verify3/logs/v3_corrupt.log`）得到 `sha before=5945dc7e… after=9466b377… equal=0`，与首轮完全一致。
- **根因**：`filemanager.cpp:12-27` 的格式定义里 CRC32 只覆盖「全局头（排除 checksum 字段）+ 条目头」，内容区没有校验值；`validateMergedFile` / `QLoadMergedFile` / `outputFile` 均不校验内容。
- **修改建议**：每个条目增加内容校验字段（CRC32 或 XXH64，写入时计算）；提供「校验归档」菜单项；导出/分割时按需校验并在损坏时明确报告「第 N 个文件内容校验失败」，而不是当作成功。若改格式，版本号升到 2 并保留 v1 读取兼容。

### P2 — 中优先级

#### P2-1 同名不同目录的条目分割时互相覆盖 【实测】

- **证据**：`run_dupname.log` —— 归档里 `dupA/same.txt`（`AAAA-FROM-DUP-A`）与 `dupB/same.txt`（`BBBB-FROM-DUP-B`），分割到同一目录后 `output files=1`，幸存内容为 `BBBB-FROM-DUP-B`。第三轮独立重跑（`verify3/logs/v3_dupname.log`）仍为 `surviving content='BBBB-FROM-DUP-B'`。
- **根因**：`filemanager.cpp:555` 用 `getFileName()` 只取 basename 作为输出名，忽略原始目录层级。
- **修改建议**：按归档内记录的相对目录层级还原；无法还原时自动加序号/短哈希后缀；冲突时提示用户选择「覆盖 / 跳过 / 自动重命名」。

#### P2-2 插件映射未实现，未知后缀无法查看 【实测】

- **证据**：`run_open_big_3.log` —— 双击 `Windows10.iso` 弹「没有插件来处理该类型文件，请手动映射插件」；`run_misc.log` —— 点击「插件映射」菜单只弹「功能开发中，敬请期待。」
- **影响**：本归档里 2 个 ISO（合计约 9 GB）完全无法查看，而提示把用户指向一个不存在的功能。
- **修改建议**：实现「后缀 → 插件」映射对话框并持久化（QSettings）；在此之前至少把提示改为「暂不支持 .iso 格式」并提供「以文本/十六进制查看」兜底入口。

#### P2-3 `validateMergedFile` 与 `QLoadMergedFile` 判定不一致 【实测】

- **证据**：`run_invalid.log` —— `pathlen_huge`（PathLength=`0xFFFFFFFF`）得到 `validate=1` 但 `loaded=0`。第三轮用独立构造的同型归档补跑 `verify3/logs/v3_v3_splitbad.log`：`validate=1` → `QSplitFiles` **正常返回**、`outputFiles=0`、全程无信号 / 无提示 —— 「静默返回、UI 无反馈」由推断升级为实测。
- **根因**：`filemanager.cpp:240-252` 的 `validateMergedFile` 只调用 `readAndValidateHeader`，不解析条目；`on_actionSplitFile_triggered`（`mainwindow.cpp:859`）依赖它做「有效性」判断，于是会放行一个 `QSplitFiles` 根本解析不了的归档，后者在 `:512-516` `break` 后直接返回（函数返回 `void`、不发 `operationFinished`）。
- **修改建议**：抽出共用的 `parseEntries()`，让校验、加载、分割三个入口用同一套解析与合法性检查；`validateMergedFile` 返回「是否可完整加载」而不是「头部 CRC 是否通过」。

#### P2-4 合并进度状态文字与进度条口径不一致 【实测；二轮复核后降级】

- **复核后定性**：进度条 **没有算错**——`emit progressChanged(copiedTotal, totalBytes, sStatus)`（`:431`）传的是累计值；只是状态文字（`:427-430`）显示「当前文件已复制 / 当前文件大小」，于是每个新文件都从 0 起跳，与进度条的全局口径对不上。这是 UI 文案一致性问题，不是进度计算 bug。
- **证据**：`run_merge.log` —— `progressSignals=4 lastStatus='正在合并: __add_probe.txt  (27 B / 27 B)'`，而本次合并的**内容数据总量是 13,848 B**（1870+94+11857+27）；日志里的 `size=14320` 是含 472 B 头部的**归档总大小**（`hdrSize=472`）。第三轮 `verify3/logs/v3_merge.log` 重跑得到完全相同的 `lastStatus`。
- **对照**：`rewriteArchive` 的 `:959-963` 文字用的是 `copiedTotal`，口径正确，可作为改法参考。
- **修改建议**：状态文字改为 `(copiedTotal / totalBytes)`，或同时显示「当前文件进度 / 总进度」。

#### P2-5 失败信息笼统 + 底层 qWarning 用户不可见 【实测 + 推断】

- 例：`run_save.log` 真实原因是「另一个句柄占用导致 rename 失败」，UI 只显示「保存失败，归档文件可能已损坏或磁盘写入失败」；`QSplitFiles` 的失败（目录创建失败、定位失败、读取不完整）全部只有 `qWarning`，`void` 返回，上层完全不知道。
- **修改建议**：把失败原因结构化（枚举/文本）通过 `operationFinished` 回传；`QSplitFiles` 也改成发 `operationFinished`；日志里给出具体路径与系统错误（`QFile::errorString()`）。

#### P2-6 ~~图片首次打开被缩到 37×28~~ → ❌ **复核后剔除：offscreen 测试环境伪影，不是真实 GUI 缺陷**

- **原观察**：`run_open_all.log` 里两张 5 MB JPG 的显示 pixmap 都是 **37×28**（PDF 页却是 547×694 / 595×842），首轮据此怀疑 `Plugins/PluginPicture/mainwindow.cpp:34-44` 的初始缩放逻辑。
- **复核实验（第二轮）**：独立 Qt 探针 `.workbuddy/tmp/fw_verify/run_viewport.log` —— 未 `show()` 的 `QScrollArea` 的 `viewport()->height()` = **28**，`resize()` 后仍是 28，**`show()` 后才变成 698**。
- **决定性对照实验（第三轮，`fw_verify3.exe`）**：同一个 `MainWindow`、同一份插件代码、同一条 `treeView->doubleClicked` 路径，**唯一变量是 `w.show()`**：
  - `verify3/logs/v3_v3_ui_hide.log`：`doShow=0 mainVisible=0` → `pixmap=37x28`
  - `verify3/logs/v3_v3_ui_show.log`：`doShow=1 mainVisible=1 mainH=820` → `pixmap=874x657`
  - 插件级对照 `v3_v3_plugin_hide/show.log`：`viewportH=28 / widgetVisible=0` → `37x28`；`viewportH=649 / widgetVisible=1` → `864x649`（原图 4624×3472）
  - 主程序 `activateTab()` 是**先**把插件窗口 `insertTab`（`:497`）、**后** `renderFile`（`:514`），真实 GUI 中主窗口已 show，插件窗口在送数据前即为 visible，viewport 高为几百像素。
- **结论**：**同一份代码只因窗口是否 show 就产生 37×28 与 874×657 的差别，坐实这是 offscreen 测试环境伪影**，不是 `m_scale` 的产品缺陷。此条从问题清单中剔除；首轮写的「全项目搜索无 resizeEvent」也不准确 —— PluginMedia 有（`:614`），只是 PluginPicture 没有。
- **仅保留为低优先级改进（未实测到实际故障）**：PluginPicture 没有 `resizeEvent` / `showEvent`，窗口尺寸变化后不会重算 `m_scale`；且 `:34` 的注释「窗口尚未显示时用原图尺寸保底」与代码不符 —— 判定条件是 `availH > 0`，而未显示时 `viewportH=28` 也大于 0，保底分支（`:41-42`）实际进不去（第三轮 `plugin_hide` 已印证）。建议补 `showEvent`/`resizeEvent`，并在 `availH` 异常小时用原图保底，同时避免覆盖用户手动缩放比例。**此项需真实 GUI 人工复核后再决定，不建议依据本报告直接修改。**

#### P2-7 EPUB 书名显示「未命名电子书」；首屏空白属文档本身 【实测，根因已精确定位】

- **证据**：`run_open_all.log` —— `从一到无穷大.epub` 显示 `未命名电子书 | 1 / 316`，正文 `QTextEdit len=1`。
- **根因（三轮实证，已从模拟升级为真实 Qt 正则）**：该 epub 的 `OPS/fb.opf` 里是 `<dc:title>从一到无穷大</dc:title>`，而 `Plugins/PluginBook/mainwindow.cpp:577-586` 的 `textOfTag()` 闭合模式是 `(?:<|:)/title`，只能匹配 `</title>` / `</:title`，**匹配不了 `</dc:title>`** → 返回空 → `:290` 回退为「未命名电子书」。第三轮把 OPF 原文提取出来，用**真实 `QRegularExpression`**（`verify3/logs/v3_v3_regex.log`）跑原正则得到 `hasMatch=0 captured=''`；换成 namespace-aware 的 `<dc:title[^>]*>(.*?)</dc:title>` 立即得到 `captured='从一到无穷大'`。标题读取确有 bug，根因确凿，不再是首轮「元数据的读取或回退可能有问题」的猜测。
- **已排除误报**：`page-001.html` 去标签后仅 8 个字符，首屏空白是文档首章为占位页，**不是解析 bug**。
- **修改建议**：修正正则以支持带命名空间前缀的闭合标签（或改用 `QDomDocument` / `QXmlStreamReader` 解析 OPF）；标题回退顺序改为 `dc:title → 文件名`；打开时自动定位到首个有正文的章节，或提示「当前为封面/版权页，共 N 章」。

#### P2-8 归档只记录绝对路径，跨机器/盘符迁移后不可用 【实测】

- 本归档 19 条路径全是 `G:/...`，当前工程在 `E:/`，G 盘原目录已不存在。
- **修改建议**：同 P0-2 第 3 点，改为相对路径 + 可选原始根；「显示路径」视图仍可展示原始路径。

### P3 — 低优先级 / 潜在风险（本次样本未直接触发，标注为推断）

#### P3-1 插件里 `qint64 → int` 强转，> 2 GiB 会截断 【代码审查推断】

- 源码：`Plugins/PluginText/mainwindow.cpp:110` `QByteArray array(szBuf, (int)nFileLen)`；
  `Plugins/PluginPicture/mainwindow.cpp:24` `pix.loadFromData(..., (int)nFileLen)`；
  `Plugins/PluginBook/mainwindow.cpp:160` `QByteArray(szBuf, (int)nFileLen)`。
  `PluginPdf` 用 `static_cast<qsizetype>`（64 位安全），`PluginMedia` 用 `av_malloc(nFileLen)`（qint64 安全）。
- 按 C++ `int32` 转换规则计算，并由第三轮 C++ 实测验证（`verify3/logs/v3_v3_intcast.log`，三值完全一致）：
  `4,907,991,040 → 613,023,744`、`4,125,556,736 → -169,410,560`、`1,711,670,246 → 1,711,670,246`（未溢出）。
  即 > 2 GiB 文件若被映射到文本/图片/电子书插件，会读到错误长度甚至负长度。
- **为何本次未触发**：> 2 GiB 的三个条目是 `.iso` / `.mp4`，分别走「无插件」与 PluginMedia；插件映射功能又未实现，无法手工改路由。
- **修改建议**：统一用 `qsizetype` / `QByteArray::size_type`；对长度先做 `if (nFileLen > std::numeric_limits<int>::max())` 检查并报错；编译时开启 `-Wconversion` 扫描同类问题。

#### P3-2 其它健壮性观察 【代码审查推断】

| 观察 | 位置 | 建议 |
|---|---|---|
| `QSplitFiles` 不校验 `contentPos`/`fileSize` 是否越界，越界时 quietly `continue` | `filemanager.cpp:549-601` | 复用 P1-1 的条目合法性检查 |
| `QMergeFiles` 无磁盘空间预检、不检查输出目录存在性 | `:256-318` | 预检并给出可读错误 |
| 合并/保存中调用 `QApplication::processEvents()`，允许用户在操作进行中再次点「合并/加载/卸载」，可能重入 | `:432`、`:964`、`:1017` | 期间禁用相关动作，或彻底移到工作线程 |
| 0 字节文件 `QGetFileContent` 返回 `nullptr`，`renderFile` / `outputFile` 只打 `qDebug`、UI 无任何提示 | `:1082-1083`、`:1150-1154` | 区分「空文件」与「读取失败」，UI 给出提示 |
| `crcData.reserve(quint32(totalHdrSize))` 上限 512 MB，会一次性分配头部大小的内存 | `:329`、`:868` | 改为流式 CRC，避免构造大头部时的大分配 |
| 分割热循环 `qDebug` 每 16 MB 一条（本次 660 条） | `:599` | 移除或节流 |

---

## 5. 建议修复顺序

| 顺序 | 问题 | 理由 |
|---|---|---|
| 1 | P0-1、P0-2 | 会真实丢失用户数据，改动小、收益最大 |
| 2 | P1-1、P1-5、P1-6 | 崩溃与状态错乱，且是其它问题的共同基础（统一解析/状态重置） |
| 3 | P1-2、P1-4（+ P1-3 架构部分） | 大文件架构调整（流式 I/O + 增量保存 + 工作线程），工作量较大，建议一并设计；P1-3 的「句柄占用」子项复核后已降级，不必单独优先修 |
| 4 | P1-7、P2-3 | 归档格式升级（加内容校验、统一解析入口），与第 3 步一起做 |
| 5 | P2-1、P2-2、P2-4、P2-5、P2-7、P2-8 | 体验与功能补全；~~P2-6~~ 已复核剔除，不属于待修问题 |
| 6 | P3-* | 静态整改与编译告警清理 |

> 提示：P1-7（内容校验）、P0-2（相对路径）涉及**归档格式变更**，建议版本号升为 2 并保留 v1 读取兼容；现有的 `1.dat` 是 v1，改格式后要能继续打开。
> 复核提示：**P2-6（图片 37×28）已由第三轮 show / 未 show 对照实验坐实为 offscreen 伪影，请不要基于它修改 PluginPicture**；如需处理「窗口缩放后图片不重算」，先在真实 GUI 下复现再决定。

---

## 6. 正常项（确认无问题，供回归基线）

- 4 GB 以上偏移：**15 个**条目的 `ContentPos` > 2^32（最大 `10,846,329,369`），全部 19 个条目的解析、定位、读取均正确（`uint64`/`qint64` 使用一致），**未发现 32 位溢出**。
- 头部 magic / version / CRC32 / count / hdrSize 的常规校验对 **7 类**畸形输入（截断、错 magic、错 CRC、count=0、count 巨大、hdrSize 巨大、version=2）均正确拒绝；另有 3 类（`pathlen_huge`、`filesize_huge`、`contentpos_huge`）头部校验通过但条目字段未校验，见 P1-1 / P2-3。
- 中文文件名（含空格、双空格 `第1章  Linux简介.wmv`）的 UTF-8 存取、列表展示、导出、分割全部正常。
- 导出 94 B / 1.8 KB / 5 MB 三个样本：大小与 SHA256 同时匹配「归档内原始字节」和「磁盘源文件」。
- 合并产物的头部 CRC、条目偏移、每个条目的内容 SHA256 全部正确。
- `1.dat` 全量分割：19 个文件全部生成，16 个小文件 SHA256 逐字节一致，3 个大文件大小一致。
- 树形/平铺/路径/名称 6 次切换中叶子数恒为 19，工具栏按钮 enable 状态逻辑正确。
- 多标签页各自独立的插件实例、切换后内容保留、关闭后计数正确。
- 卸载归档后列表清空、表头隐藏、视图按钮置灰。

---

## 7. 测试产物与后续处理

所有产物都在 `.workbuddy/tmp/`（已被 `.gitignore` 忽略），**未清理**，等你决定：

| 路径 | 内容 | 体积 |
|---|---|---|
| `.workbuddy/tmp/fw_test/` | 测试驱动源码、`fw_test.exe`、26 份 `run_*.log`、work 目录 | **约 17 GB** |
| `.workbuddy/tmp/fw_test/work/split_full/` | `1.dat` 完整分割出的 19 个文件 | 约 11 GB |
| `.workbuddy/tmp/fw_test/work/Windows10.iso`、`large_input.mp4` | 导出测试结果 | 约 4.6 GB + 1.6 GB |
| `.workbuddy/tmp/qt6_full/FileWrapper/release/fw_test.exe` | 测试驱动可执行文件 | 358 KB |
| `.workbuddy/tmp/fw_verify/` | **第二轮复核**：独立验证工程 + 5 份日志（崩溃复现、异常对照、脏标记、offscreen 视口、缓存耗时） | < 1 MB |
| `.workbuddy/tmp/verify3/` | **第三轮循环复核**：独立驱动 `verify3_main.cpp` / `fw_verify3.pro`（产物 exe 在 `qt6_full/.../release/`）+ 17 份 `logs/v3_*.log` + 引用自动核对脚本 `extract_refs.py` / `check_refs.py` + fixture 生成器 | < 1 MB |

被测试的 `1.dat` 与 `test_files/FileProtector_Input/` **全程只读，未被修改**；`FileProtector_Output/` 保持为空。测试驱动只在自己的沙箱目录内做覆盖类实验，未触碰用户源文件。

原始日志文件命名规则：`.workbuddy/tmp/fw_test/run_<case>[_<参数>].log`，例如 `run_split_full.log`、`run_invalid.log`、`run_corrupt.log`；第二轮复核日志在 `.workbuddy/tmp/fw_verify/run_*.log`，第三轮在 `.workbuddy/tmp/verify3/logs/v3_*.log`（`v3_v3_*` 为第三轮新增实验，`v3_<旧case>` 为原样重跑）。

---

## 8. 结论

功能主流程（加载、浏览、渲染、合并、分割、导出、增删保存、多标签）在 `1.dat` 上都能跑通，**正确性基线是好的**，尤其是 4 GB 以上偏移和中文路径没有出问题。

真正需要优先处理的是三类风险：

1. **数据安全**：合并自覆盖、按原路径分割静默覆盖（P0）；
2. **健壮性**：畸形头部可致崩溃、加载/卸载状态残留、加载失败后界面错乱（P1）；
3. **大文件可扩展性**：全量内存读取 + 全量重写保存 + 主线程同步分割，在 10.85 GB 归档上表现为冷读 11.7~37.4 s、导出 13.4~71.8 s、完整分割 165 s 的阻塞，以及数 GB 的内存/磁盘峰值（P1）。注意同一文件热读仅 0.59 s，性能数字须区分缓存状态。

**第二、三轮循环复核终判**：19 条问题里 **18 条确认成立、1 条剔除**（P2-6 图片 37×28 已被「show / 未 show」对照实验坐实为 offscreen 伪影）。三轮共修正 **12 处事实 / 单位 / 措辞错误**（最重要的是「>2^32 的偏移有 15 个，不是 6 个」「10.85 GB ≠ 10.85 GiB」「合并内容总量 13,848 B ≠ 归档大小 14,320 B」「加载新归档后保存按钮已置灰，不能说『点保存会重写』」「被正确拒绝的是 7 类畸形输入」），并对 P1-1 崩溃、P1-5 脏标记、P1-6 状态错乱、P2-3 静默返回、P2-7 正则、P3-1 整数截断补上独立复现证据。详见第 0 节。

仍属「推断」而非实测的只有：P1-2 的瞬时内存峰值 2×、P1-3 与 P0-2 对 10.85 GB 归档的磁盘 / 耗时外推（为避免改动用户数据未实际执行）、P3-1 / P3-2 的静态风险；正文均已写明原因。

本次**没有修改任何源码**，具体改哪些、怎么改由你决定。

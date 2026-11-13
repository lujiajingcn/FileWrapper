# FileWrapper `1.dat` 问题修复报告

> 对应基线：`TEST_REPORT_1dat.md`（三轮循环复核终判：19 条中 **18 条确认成立**、**1 条剔除**）。
> 本报告只针对 **18 条已确认成立**的问题做修复与验收；被剔除的 **P2-6（图片 37×28）没有改动任何 PluginPicture 缩放逻辑**。
> 执行时间：2026-11-13；本机 Qt 6.9.3 / MinGW 13.1.0，路径 `E:\V_C\Qt`。

---

## 0. 结论摘要

| 项目 | 结果 |
|---|---|
| 待修真实问题 | 18 条（P0×2、P1×7、P2×7〔已剔除 P2-6〕、P3×2） |
| 已修复 | **18 / 18** |
| 明确不修 | P2-6（offscreen 未 `show()` 的视口伪影，非真实 GUI 缺陷） |
| 全量编译 | 主程序 + 5 个插件 `0 error` / `0 warning`；3 个测试驱动 `0 error`（仅旧测试代码自身的历史告警） |
| 修复验收 | 17 个独立用例、**109 条断言全部 PASS / 0 FAIL** |
| 历史回归 | 18 个用例全部按预期执行，无崩溃、无 `std::bad_alloc` |
| 真实归档 | `1.dat`（10,846,329,463 B / v1 / 19 条）仍可正常加载，16 个 ≤100 MB 文件仍可全部打开 |
| Git 状态 | 改动**尚未提交、尚未推送**（用户本轮未要求提交） |

---

## 1. 修复范围与基线口径

`TEST_REPORT_1dat.md` 第 0.1b 节终判的 18 条真实问题清单：

```
P0-1 合并自覆盖          P0-2 按原路径分割静默覆盖
P1-1 畸形头部致崩溃      P1-2 大文件全量入内存
P1-3 保存全量重写        P1-4 分割无进度/取消/完成提示
P1-5 脏标记跨归档污染    P1-6 加载失败后界面状态错乱
P1-7 内容区无校验        P2-1 同名条目分割互相覆盖
P2-2 插件映射未实现      P2-3 校验入口判定不一致
P2-4 进度文案口径不一致  P2-5 失败信息笼统
P2-7 EPUB 书名解析失败   P2-8 归档只记录绝对路径
P3-1 qint64→int 截断     P3-2 其它健壮性（6 个子项）
```

P3-2 的 6 个子项（越界不校验、无磁盘预检、`processEvents` 重入、0 字节文件无提示、
头部 CRC 一次性大分配、分割热循环 `qDebug` 刷屏）全部纳入修复范围。

---

## 2. 构建与验收环境

```bash
export PATH="/e/V_C/Qt/Tools/mingw1310_64/bin:/e/V_C/Qt/6.9.3/mingw_64/bin:$PATH"
# 主程序（影子构建 qmake + mingw32-make）
cd .workbuddy/tmp/qt6_full && mingw32-make -j4
# 修复验收驱动（链接真实主程序源码）
cd .workbuddy/tmp/fixcheck && mingw32-make -f Makefile.Release -j4
# 历史回归驱动
cd .workbuddy/tmp/verify3  && mingw32-make -f Makefile.Release -j4
cd .workbuddy/tmp/fw_test  && mingw32-make -f Makefile.Release -j4
```

验收统一使用 `QT_QPA_PLATFORM=offscreen`，驱动实例化**真实** `MainWindow` / `FileManager` /
插件 / 对话框（`QPluginLoader` 加载真实 DLL），每个用例独立进程，日志位于
`.workbuddy/tmp/fixcheck/logs/`。启动脚本：`run_fix.py`（17 例）、`run_legacy.py`（18 例）。

---

## 3. 架构性改动（一次改动覆盖多条问题）

### 3.1 归档格式 v2：兼容读 v1、始终写 v2

`FileWrapper/archiveformat.h`（新增，313 行）定义公共格式：

- 全局头 24 B 不变（`FWDA` / Version / HeaderSize / FileCount / HeaderChecksum / Reserved）。
- v1 条目头 20 B：`PathLength u32 / FileSize u64 / ContentPos u64 / Path`。
- v2 头部区新增 `RootLength u32 + Root`，条目头 24 B 新增 **`ContentCrc32 u32`**，
  Path 改为**相对 Root 的相对路径**。
- `HeaderChecksum` 覆盖 `[0..15] + [20..23] + 头部区[24..HeaderSize)`，
  由 `Crc32` **流式分块**计算，不再一次性 `reserve(HeaderSize)`。
- 新增安全工具：`isSafeRelativePath()`（拒绝绝对路径、`..`、控制字符、非法字符、保留设备名）、
  `sanitizeRelativePath()`、`commonRoot()` / `makeRelative()` / `displayPathFor()`、
  `isValidUtf8()`、`availableBytes()`、`formatSize()`。

> 兼容结论：现有 `1.dat` 是 v1，不改后缀、不改内容即可继续打开（验收 `v1load` 9/9 PASS、
> 历史回归 `load` 得到 `entries=19`）。v1 无内容校验字段，`verifyArchive()` 会明确返回
> 「当前归档是 v1 格式…建议用『保存』重写为 v2 格式后即可校验」，而不是伪报通过。

### 3.2 统一解析入口 `parseFromDevice()`

`filemanager.cpp:57`。所有入口共用同一份**完整解析 + 合法性校验**：

- `parseFile()` / `inspectArchive()` / `validateMergedFile()` / `QLoadMergedFile()` /
  `QSplitFiles()` / `QMergeFiles()` 不再各自只读一个头。
- 校验项：魔数、version、HeaderSize、FileCount、头部 CRC32、`RootLength`、
  逐条 `PathLength` / `FileSize` / `ContentPos`、内容区是否越界、**内容区是否重叠**、
  UTF-8 合法性、相对路径安全性。
- 读取方式：流式逐条读取，不缓存整个头部区；伪造成 `2^63-1` 的 `FileSize` 在进入任何
  `new[]` 之前就被拒绝。
- v2 墓碑（快速删除）识别：`PathLength` 保持原值，**路径区首字节为 NUL** 即视为已删除槽位，
  整条跳过（槽位大小不变，后续条目才能正确对齐）。

### 3.3 流式 I/O 与进度/取消

- `streamCopy()`（`:620`）：固定 1 MB 缓冲分块复制，同时算 CRC、发 `progressChanged`、
  检查 `m_bCancelRequested`、调 `processEvents()` 保持 UI 响应。
- `progressChanged(qint64, qint64, QString)` / `operationFinished(bool, QString)` /
  `warningOccurred(QString)` / `cancelOperation()` 全部接通 `QProgressDialog`。
- `BusyGuard` + `isBusy()` + `setOperationsEnabled(false)`（主窗口 `:1253`）在耗时操作期间
  禁用菜单/工具栏，消除 `processEvents()` 期间的重入。
- `beginProgressOperation()` / `endProgressOperation()`（`:1269`、`:1292`）只管理三条连接，
  不再用 `disconnect(m_fileManager, nullptr, this, nullptr)` 误断 `warningOccurred` 等常驻连接。

### 3.4 大文件「临时文件 + 路径」通道（插件 ABI 向后兼容）

`plugininterface.h` 以**带默认实现的虚方法**扩展接口，旧插件无需重编即可继续工作：

```cpp
virtual bool prefersFileByPath() const { return false; }
virtual bool sendFileByPath(const QString &path, const QString &displayName) { ... return false; }
virtual void setSourceName(const QString &name) { Q_UNUSED(name); }
```

- `PluginMedia`：`showVideoFile(path)` 让 FFmpeg 直接 `avformat_open_input(路径)`；
  `PluginPdf`：`showPdfFile(path)` 让 `QPdfDocument::load(path)` 流式打开。
- `MainWindow::renderFile()`（`:599`）：文件 ≥ 128 MB 且插件支持路径通道时，先
  `extractEntryToTempFile()` 流式抽取到临时文件（内存固定 1 MB），再 `sendFileByPath()`；
  插件拒绝时自动回退内存通道。
- 临时资源保护（本轮新增）：同一条目重复打开**复用**已抽取副本（`m_mapTempCache`），
  最多保留 8 个副本（`m_listTempOrder` 上限淘汰），切换归档时 `closeAllTabs()` 调用
  `discardTempCache()` 立即回收；启动时只清理 24 小时前的陈旧 `fw_*`，不误删另一个实例的文件。

---

## 4. 18 条问题逐条修复对照

| 编号 | 问题 | 修复要点 | 主要文件 | 验收用例 / 结果 |
|---|---|---|---|---|
| P0-1 | 输出=输入静默清空源文件 | canonical/absolute 比较，UI 层 + `FileManager` 双层拦截；目录输入用 `QDirIterator` 展开；输出已存在二次确认；临时文件 + 校验 + 备份 + rename 原子替换 | `dlgmergefile.cpp:96`、`filemanager.cpp:664` | `selfoverwrite` 5/5、`merge_ui_self` 5/5 |
| P0-2 | 按原路径分割覆盖已改动的源文件 | 默认输出到用户选定目录；按原路径需二次确认并统计绝对路径数与总大小；不静默覆盖、自动重命名、路径回溯拒绝 | `mainwindow.cpp:1032`、`filemanager.cpp:1110/1162` | `oldpath` 5/5、`split_dup` 5/5 |
| P1-1 | 伪造 `fileSize` 触发 `std::bad_alloc` 崩溃 | 统一完整解析 + 条目字段合法性校验，进入 `new[]` 前拒绝；`QGetFileContent` 用 `new (std::nothrow)` | `filemanager.cpp:57/1297` | `badheader` 9/9；`v3_nocatch` 不再崩溃 |
| P1-2 | 大文件全量入内存 | 路径通道 + 流式抽取；`QGetFileContent` 分块 + 超大长度明确报错「请改用导出或分割」；`outputFile()` 流式导出 | `mainwindow.cpp:544/599`、`filemanager.cpp:304/1297/1388` | `pluginpath` 5/5（真实 PluginMedia.dll 按路径打开 MP4） |
| P1-3 | 保存全量重写 / rename 原因不可见 | v2 **墓碑式快速删除**：只改头部标记 + 重算头部 CRC，内容区 0 字节改动；头部一次读入内存、单次写回、失败整体回滚；rename 失败给出「可能被其它程序占用」具体原因；磁盘空间预检 | `filemanager.cpp:834/971` | `fastdelete` 16/16（两次原地删除大小不变、CRC 校验通过、新增后重写回收空间） |
| P1-4 | 分割 165 s 无进度/取消/完成提示 | 分割全程 `progressChanged`、`QProgressDialog` 取消按钮接 `cancelOperation()`、`operationFinished` 汇总「成功/跳过/失败/自动重命名」计数；移除热循环 `qDebug` | `filemanager.cpp:1162`、`mainwindow.cpp:1032` | `splitprogress` 5/5；历史 `split_ui` 汇总文案实测 |
| P1-5 | 脏标记与待添加集合跨归档污染 | `QLoadMergedFile()` 解析成功后才切换句柄/状态，并清空 dirty / pending / cancel / meta；`unLoadMergedFile()` 同样完整重置 | `filemanager.cpp:367/410` | `state` 6/6；历史 `v3_dirtyui` 卸载后不再弹「未保存改动」误报路径 |
| P1-6 | 加载无效归档后当前归档被卸载、保存按钮仍亮 | 加载前先 `validateMergedFile()` 完整解析，失败即返回且**不动**旧归档/视图/脏标记；`syncActionStates()` 统一刷新所有动作状态 | `mainwindow.cpp:802/1234`、`filemanager.cpp:283/367` | `loadfail` 5/5；历史 `add_del` 保留 20 行与 `saveEnabled=1` |
| P1-7 | 内容区无校验，损坏数据被静默导出 | v2 每条目 `ContentCrc32`；导出/打开/抽取/分割/校验时逐条比对；新增「校验归档内容」菜单 | `archiveformat.h`、`filemanager.cpp:1489` | `merge_v2` 9/9、`corrupt_v2` 10/10 |
| P2-1 | 同名不同目录条目分割互相覆盖 | `chooseOutputPath()` 按相对层级重建目录 + 自动重命名，不静默覆盖 | `filemanager.cpp:1110` | `split_dup` 5/5 |
| P2-2 | 插件映射「功能开发中」 | 新增 `DlgPluginMap`（显式 `QSettings(IniFormat, UserScope, "FileWrapper", "FileWrapper")`）+ `BuiltinViewer`（文本/十六进制，最多显示 1 MB）；未知后缀弹框可选查看器/映射/取消 | `dlgpluginmap.cpp/.h`、`builtinviewer.cpp/.h`、`mainwindow.cpp:341/963/1571` | `pluginmap` 4/4；`smoke` 菜单项实测存在 |
| P2-3 | `validateMergedFile` 只校验头 | 所有入口统一走 `parseFromDevice()` 完整解析；畸形归档在分割前被拒绝并给出具体原因 | `filemanager.cpp:283/293/1162` | `splitbad` 3/3（解析失败 → `outputFiles=0` 且无静默返回） |
| P2-4 | 进度文案「当前文件进度」与进度条累计口径不一致 | 全部改为累计口径 `已复制 / 总内容量`，文案统一带归档格式大小 | `filemanager.cpp:664/620` | `mergetext` 3/3 |
| P2-5 | 失败信息笼统、底层 `qWarning` 用户不可见 | `lastError()` 保留具体原因；`warningOccurred` 接状态栏；保存/合并/分割/导出失败均展示真实错误与可重试提示 | `filemanager.cpp:26/33`、`mainwindow.cpp:201/1470/1516` | 各用例错误信息断言（如 `selfoverwrite`、`loadfail`、`splitbad`） |
| P2-6 | 图片 37×28 | **不修改**（已坐实为 offscreen 未 `show()` 的 viewport 伪影） | 无 | — |
| P2-7 | EPUB 书名显示「未命名电子书」 | `textOfTag()` 改 namespace-aware 正则，可匹配 `</dc:title>`；新增 `fallbackTitle()` 回退源文件名；`setSourceName()` 经插件接口传入 | `Plugins/PluginBook/mainwindow.cpp`、`pluginbook.cpp`、`plugininterface.h` | `epub` 5/5（真实 DLL 解析出「从一到无穷大」） |
| P2-8 | 只记录绝对路径，跨机器不可用 | v2 写入相对路径 + 头部 `Root`；v1 读取时由展示路径推导 `commonRoot()` 并生成安全相对路径（层级 ≥3 退化为纯文件名） | `archiveformat.h`、`filemanager.cpp:248` | `merge_v2` 9/9（3/3 相对路径）、`v1load` 9/9 |
| P3-1 | 插件中 `qint64 → int` 截断 | `PluginText` / `PluginPicture` / `PluginBook` 统一 `qsizetype`，并对超过 `qsizetype::max()` 的长度给出明确提示 | 三个插件的 `mainwindow.cpp` | 静态代码审查（未用 >2 GiB 真实文件触发，见第 7 节） |
| P3-2 | 6 项健壮性观察 | ① 越界校验并入统一解析 ② 磁盘空间预检 + 输出目录检查 ③ `BusyGuard`/动作禁用 ④ 0 字节文件与读取失败区分并提示 ⑤ 流式 CRC 取消大分配 ⑥ 移除分割热循环 `qDebug` | `filemanager.cpp` 全篇、`mainwindow.cpp:1253` | `badheader`、`splitbad`、`v3_nocatch` 等；`grep qDebug filemanager.cpp` 为空 |

---

## 5. 关键实现细节

### 5.1 v2 墓碑式快速删除（P1-3 的核心）

原实现把 `PathLength` 写成 0，会破坏后续条目的字节对齐；且「多次零散写 + 失败即留下半写头部」
会在写标记成功但回填 CRC 失败时留下**坏归档**。本轮改为事务性写法：

```
1. 关闭自身只读句柄 → QFile(ReadWrite) 打开归档
2. 头部区一次性读入内存（上限 64 MB，超过则回退流式完整重写）
3. 内存快照 hdrOrig 用于回滚
4. 遍历物理槽位：把「不在 keep 集合」的条目路径区首字节改为 NUL
   同时校验 matched == keep.size()、拒绝把全部条目标记为删除
5. 在内存里重算 HeaderChecksum 并回填 [16..19]
6. 单次连续 write 回位置 0 → flush
7. parseFile() 重新完整解析，存活条目数必须等于预期；否则整体回滚 hdrOrig
8. QLoadMergedFile() 刷新偏移、清 dirty / pending，并重开自身句柄
```

`FileCount` 仍是物理槽位数（含墓碑），可能大于可见条目数；内容区留到下一次完整重写时回收。
解析器与写入器的墓碑判定完全一致（首字节 NUL），并兼容早期 `PathLength=0` 的旧写法。

验收 `fastdelete`：6 MB / 3 条目的 v2 归档，删第 0 条后 `大小 6,291,672 B 不变`、
`verifyArchive` 通过；再删一条仍为原地标记；新增文件后完整重写，归档变小、CRC 全通过。

### 5.2 合并/保存的原子替换（P0-1 / P1-3）

写临时 `.tmp.fwda` → 写完回填头部 CRC → `parseFile()` 校验临时文件 → 关闭自身句柄 →
原文件 rename 为 `.bak.fwda` → 临时文件 rename 为正式文件 → 删除备份。任一步失败都恢复备份、
保留内存中的 dirty 改动供重试，并给出「文件可能被其它程序占用」等具体原因。

### 5.3 未知后缀处理（P2-2）

`resolvePlugin()` 不再只提示「请手动映射插件」，而是弹出三选一：内置文本查看 /
内置十六进制查看 / 打开插件映射对话框 / 取消；`choosePlugin()` 优先读
`DlgPluginMap::mappingFor()`，支持映射到 `builtin://text`、`builtin://hex` 或真实插件 DLL 名。

---

## 6. 验收结果（可核验证据）

### 6.1 编译

| 目标 | 结果 |
|---|---|
| `qt6_full`（主程序 + 5 插件） | `FULL_EXIT=0`，`0 error`、`0 warning` |
| `fw_fixcheck.exe` | `BUILD_EXIT=0` |
| `fw_verify3.exe` / `fw_test.exe` | 均 `EXIT=0`（仅旧测试代码自身的 unused-parameter / deprecated 告警） |

### 6.2 修复验收 17 例 / 109 断言

```
### v1load           EXIT=0  PASS=9   FAIL=0      ### split_dup        EXIT=0  PASS=5  FAIL=0
### selfoverwrite    EXIT=0  PASS=5   FAIL=0      ### oldpath          EXIT=0  PASS=5  FAIL=0
### merge_ui_self    EXIT=0  PASS=5   FAIL=0      ### badheader        EXIT=0  PASS=9  FAIL=0
### merge_v2         EXIT=0  PASS=9   FAIL=0      ### state            EXIT=0  PASS=6  FAIL=0
### corrupt_v2       EXIT=0  PASS=10  FAIL=0      ### loadfail         EXIT=0  PASS=5  FAIL=0
### splitbad         EXIT=0  PASS=3   FAIL=0      ### splitprogress    EXIT=0  PASS=5  FAIL=0
### mergetext        EXIT=0  PASS=3   FAIL=0      ### fastdelete       EXIT=0  PASS=16 FAIL=0
### pluginmap        EXIT=0  PASS=4   FAIL=0      ### epub             EXIT=0  PASS=5  FAIL=0
### pluginpath       EXIT=0  PASS=5   FAIL=0
```

新增的 `merge_ui_self` 用真实 `DlgMergeFile` 验证 UI 层拦截：
日志出现 `输出文件不能是待合并文件之一：… victim.bin`，对话框未被 `accept()`，
源文件 SHA 未变。

### 6.3 历史回归 18 例

`smoke / load / views / tabs / open_all / misc / export18 / invalid / add_del / save /
merge_ui / split_small / split_ui / v3_nocatch / v3_dirtyui / v3_splitbad / v3_ui_show / v3_regex`
全部按预期执行。其中：

- `load`：`1.dat` 加载成功、`entries=19`，v1 兼容性保持。
- `open_all`：16 个文件全部打开，插件实例正常。
- `invalid`：7 类畸形输入全部被拒绝，另有 3 类（伪造 `pathlen/filesize/contentpos`）
  由新的条目级校验拦下。
- `v3_nocatch`：`EXIT=1` 是**旧用例自身逻辑**——它原本用于证明「无 try/catch 时进程崩溃」，
  代码为 `if (infos.isEmpty()) return 1;`。现在畸形归档被正确拒绝 → `infos` 为空 → 返回 1。
  关键变化是日志中**不再出现** `terminate called after throwing an instance of 'std::bad_alloc'`，
  进程正常退出而非崩溃。

---

## 7. 兼容性、边界与未实测项

1. **v1 首次保存仍需完整重写**：`1.dat` 是 v1，升级到 v2 必须重写 10.85 GB，这是格式升级的
   固有代价；墓碑快速删除只对已升级为 v2 的归档生效。为避免改动用户原始数据，本轮**未对
   `1.dat` 实际执行保存**。
2. **P1-2 的瞬时内存峰值 2×**、**P1-3 / P0-2 对 10.85 GB 归档的磁盘与耗时外推**：沿用
   测试报告口径，仍属源码推断，未做破坏性实测。路径通道的接口与小样本 MP4 已实测通过，
   但**未用 >2 GiB 的真实归档条目做端到端抽取 → 播放**实测。
3. **P3-1 的 `qint64→int` 截断**：三处已改为 `qsizetype` 并加入长度上限检查，属静态代码
   审查修复；未构造 >2 GiB 的文本/图片条目触发原崩溃路径。
4. **P2-6 未改动**：按三轮复核结论，PluginPicture 缩放逻辑保持原样，避免基于误报修改。
5. **临时文件上限 8 个**：淘汰时若文件仍被插件占用（Windows 删除会失败），该文件保留在
   `m_setTempFiles` 中，析构时再清；不会无限增长，但极端情况下单会话可能短暂超过 8 个。
6. **两个 Qt6 弃用告警**：`QString::fromUtf16(ushort*)` 属于历史遗留，与本次 18 条问题无关，
   本轮未处理（全量增量编译本次 0 warning，是因为相关翻译单元未重编）。

---

## 8. 改动文件清单

**新增**

| 文件 | 行数 | 作用 |
|---|---|---|
| `FileWrapper/archiveformat.h` | 313 | v1/v2 格式定义、CRC32、路径与磁盘工具 |
| `FileWrapper/builtinviewer.cpp/.h` | 102 / 33 | 内置文本 / 十六进制查看器 |
| `FileWrapper/dlgpluginmap.cpp/.h` | 164 / 35 | 后缀 → 插件映射对话框与 `QSettings` 持久化 |
| `.gitignore` | 72 | 构建产物忽略规则（保留 vendored 源码 / 依赖库 / 测试源码） |
| `TEST_REPORT_1dat.md` | 444 | 三轮复核后的最终测试报告 |
| `.workbuddy/tmp/fixcheck/*` | — | 17 例修复验收驱动、脚本与日志（已被 `.gitignore` 忽略） |

**修改**

| 文件 | 关键改动 |
|---|---|
| `FileWrapper/filemanager.cpp/.h` | 统一解析、v2 写入、流式 I/O、CRC、进度/取消、墓碑快速删除、安全输出路径、`lastError()`/`isBusy()`/`verifyArchive()`/`inspectArchive()`/`extractEntryToTempFile()` |
| `FileWrapper/mainwindow.cpp/.h` | `syncActionStates()`、重入保护、进度对话框连接管理、路径通道与临时文件缓存、校验归档菜单、加载失败保持原状态、分割二次确认、插件映射、导出/保存错误提示 |
| `FileWrapper/plugininterface.h` | 三个带默认实现的可选虚方法，保持旧插件 ABI 兼容 |
| `FileWrapper/dlgmergefile.cpp` | 输出=输入 UI 层拦截、目录输入展开、已存在二次确认 |
| `FileWrapper/FileWrapper.pro` | `CONFIG += c++17`，纳入新增源/头文件 |
| `Plugins/PluginMedia/mainwindow.cpp/.h`、`pluginmedia.cpp/.h` | `showVideoFile()` + `sendFileByPath()` 路径通道 |
| `Plugins/PluginPdf/mainwindow.cpp/.h`、`pluginpdf.cpp/.h` | `showPdfFile()` + `sendFileByPath()` 路径通道 |
| `Plugins/PluginBook/mainwindow.cpp/.h`、`pluginbook.cpp/.h` | namespace-aware 正则、书名回退、`setSourceName()`、`qsizetype` |
| `Plugins/PluginText/mainwindow.cpp` | `qsizetype` 防截断 + 超大长度提示 |
| `Plugins/PluginPicture/mainwindow.cpp` | `qsizetype` 防截断 + 超大长度提示（**不含**缩放逻辑改动） |
| `Plugins/PluginPdf/PluginPdf.pro` | `CONFIG += c++17` |

---

## 9. 当前 Git 状态

`git status --short` 显示上述文件为 `M` / `??`，分支 `main`，
最近提交仍为 `9b882aa`（gapless 时间戳修复）与 `bd93f9b`（视频 seek 修复）。

> **本轮未执行 `git commit` / `git push`**：用户当前指令是「修复全部已发现的真实问题」，
> 未要求提交。如需提交并推送，请明确指示。

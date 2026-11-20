# FileWrapper 第四轮审计修复复查报告

- 日期：2026-11-20
- 对应主报告：`CODE_AUDIT_CYCLE4_2026-11-20.md`
- 修复基线：`HEAD = 6f67515` + 本轮工作区改动；未 `git commit`、未 `git push`
- 修复范围：27 项确认问题中的 27 项（P1×1、P2×9、P3×17）；另有 1 项未实证
  （播放中音频设备热拔）在第五节列出。C4-23（接口 v7 非阻塞复查）、C4-24
  （QThreadPool 缩略图）、C4-25（OPC fixture）、C4-26（runner 超时）、C4-27
  （ODT repeat）为收尾后按用户要求补做的修复，已并入本报告。

## 一、验证总览

| 验证项 | 命令/方式 | 结果 |
|---|---|---|
| 全量构建（主程序 + 8 插件） | `_bw/audit4`：`qmake ..\..\FileWrapper.pro -spec win32-g++ "CONFIG+=debug"` + `mingw32-make -j6` | **0 error**；工程代码 0 warning；47 条警告全部来自 vendored libmobi |
| 部署依赖 | 构建末尾 `deploy-deps.ps1` | `copied 23 file(s)`，`declared 23 entr(ies), warnings 0` |
| 回归套件（未启用截图） | 独立 `cmd.exe` 子进程：`plugin_tests.exe <fixtures> <app-plugins> <log> --integrate` | **242 PASS / 0 FAIL / 3 SKIP**，退出码 0 |
| 回归套件（启用截图） | 同上 + `--shots _bw/audit4/shots` | **245 PASS / 0 FAIL / 0 SKIP**，退出码 0 |
| 回归套件（官方 runner） | `tests\run-plugin-tests.ps1 -SkipProjectBuild -AppBuildDir _bw\audit4 -SkipShots` | **242 PASS / 0 FAIL / 3 SKIP**，`exit : 0` |
| runner 超时 | 同 runner + `-TimeoutSec 1` | `TIMEOUT after 1s`、`exit : 124` |
| 修复前基线 | 同一影子构建、同一命令 | 238 PASS / 0 FAIL / 4 SKIP（截图关闭） |
| package.ps1 语法 | PowerShell Parser | `PACKAGE_SYNTAX_OK` |
| package.ps1 子目录守卫 | 只读路径复算 | `.git`/`FileWrapper`/仓库根均 `blocked=True`；盘根由盘根守卫拦截 |
| 写锁存活判定 | 新增 A4：活 PID + 40 秒旧 mtime、死 PID + 40 秒旧 mtime | 活 PID 仍拒绝；死 PID 清理后合并成功；`AUDIT-P0-1` PASS |
| ZIP 声明 0 字节 | 新增 A11 扩展 + 独立探针 | 旧探针 `out=1048576 total=0`；修复后空结果且不再计 0 预算；A11 PASS |
| XML 属性放大 | 新增 A12 + 20 万属性探针 | 修复前 `setContent=66.7s`；修复后 `xmlWithinTagBudget` 0 ms 拒绝；A12 PASS |
| 接口 v7 | W6 / W8a / W1 | `8 sidecar(s), all apiVersion=7 (bad=0)`；`apiVersion=7 accepted`；插件全部正常加载 |
| 缩略图线程化 | H21a-d + 运行期自检 | H21 全部 PASS；“解码任务在 GUI 线程”告警 0 次 |

> 环境说明：本机沙箱的 `tests/build/` 被旧 ACL 的
> `Everyone: DeleteSubdirectoriesAndFiles=Deny` 锁住，qmake 无法替换已有
> `Makefile.Debug`，官方 `tests\run-plugin-tests.ps1` 在该目录上会报
> “qmake failed”。为取得可信结果，本轮用同一 `tests/plugin_tests.pro` 在
> `_bw/audit4/testsuite` 影子构建，**没有改弱测试**；`Start-Process` 被沙箱拒绝后
> 改用独立 `cmd.exe` 子进程并分别重定向 stdout/stderr，等价于“独立子进程读退出码”。

## 二、逐项修复与验证

### P1

#### C4-01 package.ps1 禁止删除仓库内子目录 — 已修复
- 改动：`FileWrapper/deploy/package.ps1:60-66` 增加
  `outFull` 位于 `repoRoot` 内的判断；保留原有的“仓库根/祖先/盘根/构建目录”检查。
- 验证：只读复算
  `E:\...\FileWrapper\.git -> blocked=True`、
  `E:\...\FileWrapper\FileWrapper -> blocked=True`、
  `E:\...\FileWrapper -> blocked=True`。未实际执行删除。
- 结论：已修复。

### P2

#### C4-02 缩略图内存上限 — 已修复
- 改动：`FileWrapper/mainwindow.cpp:79-89` 解码尺寸 192→128；
  新增 `kThumbnailMaxGenerated=1024`；`populateFilePathThumbnails()` 用
  “已缓存 + 已生成”计数；`processNextThumbnail()` 达上限即停并状态栏提示；
  `mainwindow.h:298` 新增计数成员。
- 验证：修复前探针 1200 张图片 RSS +176 MB；修复后同一路径单张约 64 KiB，
  总量封顶约 64 MB。H21a-d 全部 PASS。
- 结论：已修复。

#### C4-03 写锁 PID 存活判定 — 已修复
- 改动：`FileWrapper/filemanager.cpp:179-270` 新增 `isProcessAliveForLock()`
  （Windows `OpenProcess`+`GetExitCodeProcess`，Unix `/proc`）；30 秒后仅当 PID
  已退出才删锁；析构只删自己 PID 的锁。
- 验证：A4 扩展用例通过：活 PID + 40 秒旧 mtime 仍拒绝第二个写者；死 PID +
  40 秒旧 mtime 可清理并完成合并。
- 结论：已修复。

#### C4-04 FB2 递归深度 — 已修复
- 改动：`Plugins/PluginBook/mainwindow.cpp:166,730,869-903` 增加 `nDepth` 参数与
  `kMaxXmlDepth=200`；超限 `markTruncated` 并停止递归；头文件同步。
- 验证：1500 层 28.6 KB FB2 探针修复前 `0xC00000FD` 栈溢出，修复后正常退出并显示
  截断原因；现有 Book 测试 V3 通过。
- 结论：已修复。

#### C4-05 XML 属性放大 — 已修复
- 改动：`Plugins/common/xmlguard.h:15-93` 线性扫描同时限制单元素属性数
  （4096）并跳过注释/CDATA/PI；保留 500k 标签总预算与函数签名。
- 验证：20 万属性/2.29 MB 探针修复前 `setContent` 66.7 秒，修复后预算 0 ms 拒绝；
  3000 属性文档通过；A12 用例 PASS；Word/Sheet/Slides/Book 现有渲染用例全部 PASS。
- 结论：已修复。

#### C4-06 EPUB 解压次数 — 已修复
- 改动：`Plugins/common/zipreader.h` 增加 `entryCount()`；
  `Plugins/PluginBook/mainwindow.cpp:74-120,519-520,680-681` 按条目数放宽
  `maxExtractions`，并把“预算耗尽”与“0 字节条目”区分，失败时回传原因、标记截断。
- 验证：2052 条目 EPUB 修复前 1 章、修复后 2 章；container 放第 2048 条之后可打开；
  现有 V3 EPUB 测试 PASS。
- 结论：已修复。

#### C4-07 ZIP 声明 0 字节的预算绕过 — 已修复
- 改动：`Plugins/common/zipreader.cpp:260-350`：`uncompSize==0` 且 `compSize!=0`
  直接拒绝；解压按声明长度一次分配，输出超出/长度不符即拒绝，去掉 6 次放大重试。
- 验证：1 MiB 实际内容、声明 0 字节的探针修复前连续 5 次返回 1 MiB 且
  `budget.totalBytes=0`；修复后空结果并在首次拒绝；A11 扩展 PASS。
- 结论：已修复。

#### C4-08 package.ps1 漏复制 translations — 已修复
- 改动：`FileWrapper/deploy/package.ps1:87` 复制列表加入 `translations`。
- 验证：构建目录 `translations/qtbase_zh_CN.qm` 存在；清单 23 项中该项现在会被复制；
  `main.cpp` 已从 `<exe>/translations` 加载。未执行完整 windeployqt（沙箱限制），
  按复制范围静态复算无遗漏。
- 结论：已修复。

#### C4-09 插件 JSON 的重建约定 — 已修复（文档/约定）
- 改动：`Plugins/plugin_common.pri:63-75`、`AUDIT_2026-11-19_FIX_REPORT.md`、
  `.workbuddy/memory/MEMORY.md` 明确：DISTFILES 不是 moc 依赖，改 JSON 后必须删除
  该插件的 `moc_*.cpp`/对象或 `mingw32-make clean`，再 qmake+make。
- 验证：检查生成的 `Makefile.Debug` 中 `moc_plugintext.cpp` 依赖不含 JSON；
  现有构建未受影响。
- 结论：已修复文档约定；自动依赖仍为后续构建系统改进项，未假装已实现。

#### C4-10 停播未确认时保留映射 — 已修复
- 改动：`FileWrapper/mainwindow.cpp:2399-2415` `releaseMappedInput()` 命中
  `m_setStopUnsafe` 时直接返回并告警，与 `releaseAllMappedInputs()` 一致。
- 验证：代码路径核对；V13 媒体停止/重播用例 PASS；未构造 30 秒卡死真实 FFmpeg
  场景（需病态解码器），修复按 P0-3 不变量收口。
- 结论：已修复。

### P3

#### C4-11 插件析构先 requestStop + 共享音频成员 — 已修复
- 改动：`Plugins/PluginMedia/mainwindow.cpp:1444-1454` 析构先 `requestStop()`
  再等待；`mainwindow.h`/`cpp` 的 `m_audioMutex`/`byteBuf` 改 `std::shared_ptr`，
  worker 持有所有权；相关使用点全部改 `.get()`/`->`。
- 验证：暂停中删除探针修复前 3006 ms + “保留 worker”；修复后立即退出、正常释放；
  V13 系列 PASS；全量构建 0 error/0 project warning。
- 结论：已修复。

#### C4-12 seek 超时中断标志 — 已修复
- 改动：`Plugins/PluginMedia/mainwindow.cpp:1731-1751` 失败分支复位
  `m_ioInterrupt`；仅确认 worker 已退出才清停止请求。
- 验证：代码路径核对；V 组媒体用例 PASS；未构造 2 秒慢 I/O 真实场景。
- 结论：已修复。

#### C4-13 map 失败不读旧 lastError — 已修复
- 改动：`FileWrapper/mainwindow.cpp:2087-2100` 只在 `pMapped != nullptr` 且插件
  返回 false 时读 `lastError()`；map 失败直接走临时文件回退。
- 验证：代码路径核对；A11 宿主文案用例 PASS。
- 结论：已修复。

#### C4-14 Slides/Sheet 单部件失败标记截断 — 已修复
- 改动：`PluginSlides/slidesdocument.cpp:392-407`、
  `PluginSheet/sheetdocument.cpp:399-414` 在跳过页/表前 `markTruncated`；
  全部失败时 `open()` 返回 false 并附原因。
- 验证：插件探针：2 页 pptx 第 2 页超预算 → `truncatedReason="第 2 页 XML 元素过多，已跳过"`；
  2 表 xlsx 第 2 表超预算 → 有“工作表「S2」…”提示；现有 Slides/Sheet 渲染用例 PASS。
- 结论：已修复。

#### C4-15 PDF 失败原因 — 已修复
- 改动：`Plugins/PluginPdf/mainwindow.cpp` 新增 `pdfErrorText()`、`showLoadError()`；
  `showPdf` 返回 bool；`pluginpdf.{h,cpp}` 实现 `lastError()`/`clearLastError()`；
  `pageCount()<=0` 返回 false。
- 验证：garbage PDF 探针修复前 `labels=[空文档]`、返回 true；修复后返回 false、
  `lastError()` 非空、内容区显示“不是有效的 PDF 文件…”。
- 结论：已修复。

#### C4-16 EPUB 裸 `%` — 已修复
- 改动：`Plugins/PluginBook/mainwindow.cpp:33-38` 改用 `percentDecodeStrict`。
- 验证：`100%.png` 严格解码保持原值；裸 `%` 单章 EPUB 探针修复前丢章/报损坏，
  修复后正常显示；现有 V3 EPUB 用例 PASS。
- 结论：已修复。

#### C4-17 确认保存后的统一刷新 — 已修复
- 改动：`FileWrapper/mainwindow.cpp:3002-3007` 新增 `afterArchiveSaved()`；
  Ctrl+S 与 `confirmDiscardOrSaveIfDirty()` 保存成功后共用；头文件声明。
- 验证：代码路径核对（确认保存 → 取消加载对话框 → 列表/标签页偏移已刷新）；
  现有 K 组保存路径用例 PASS。
- 结论：已修复。

#### C4-18 扁平归档添加去重 — 已修复
- 改动：`FileWrapper/filemanager.cpp:926-953` 添加前建立规范化键集合
  （`absoluteFilePath` 归一化、盘符冒号转首段、小写），命中即跳过。
- 验证：现有 K/G 组合并用例 PASS；未构造 v1 扁平归档重复添加的 UI 探针，
  逻辑与 `computeStoredPaths` 的保存键一致。
- 结论：已修复。

#### C4-19 RTF 截断记账 — 已修复
- 改动：`PluginWord/rtfdocument.{h,cpp}` 新增 `m_truncated`/`isTruncated()`，
  段落/字符上限置位；`worddocument.cpp:1166-1177` 传递 truncated 并在 HTML 末尾
  追加截断说明。
- 验证：60000 段 RTF 探针修复前 `truncated=false`、无提示；修复后
  `truncated=true` 且有“仅显示前一部分内容”；现有 RTF 用例 PASS。
- 结论：已修复。

#### C4-20 测试/记忆基线 — 已修复
- 改动：`tests/README.md:3-8` 更新为 247 项（无设备；有设备 245）、
  本机 242+3（截图关闭）/245+0（截图）
  基线；`.workbuddy/memory/MEMORY.md` 同步基线、打包守卫、锁语义、JSON 构建要求、
  ZipReader/缩略图约束。
- 验证：实跑日志与文档数字一致（见第一节）。
- 结论：已修复。

#### C4-21 插件接口虚函数注释 — 已修复
- 改动：`include/plugininterface.h:19-22` “10 个虚函数” → “13 个虚函数（另有虚析构）”。
- 验证：grep `virtual` 计数 13；`PLUGIN_API_VERSION` 仍 6，8 个 JSON 仍 6，未升版本。
- 结论：已修复。

#### C4-22 根 README / runner ASCII — 已修复
- 改动：`README.md` 增加构建/部署/测试/打包入口与插件 JSON 清理规则；
  `tests/run-plugin-tests.ps1:158` 中文注释改英文。
- 验证：README 链接的路径存在；runner 仍为 ASCII（除头注释外无新中文）；
  PowerShell 语法未变。
- 结论：已修复。

#### C4-23 停播超时后的永久降级 — 已修复（接口 v7）
- 改动：`include/plugininterface.h` 新增
  `virtual bool isPlaybackStopped() const`，默认 `!hasBackgroundPlayback()`；
  `PLUGIN_API_VERSION` 6→7；8 个插件 JSON 同步 `apiVersion:7`。
  `FileWrapper/mainwindow.{h,cpp}` 新增 1 秒 `m_stopUnsafePoll` 与
  `pollStopUnsafeInstances()`：非阻塞查询 unsafe 实例，成功后退还映射、
  销毁延期实例、恢复 `PluginManager::setUnloadEnabled(true)`；保存/重载入口
  先主动 poll 一次。`Plugins/PluginMedia` 用 `VideoWorker::isDecodingActive()`
  实现原子查询。
- 验证：clean 构建 **0 error / 0 warning**；W6
  `8 sidecar(s), all apiVersion=7 (bad=0)`、W8a `apiVersion=7 accepted`、
  W1 插件全部正常加载；全量回归 **245 PASS / 0 FAIL / 0 SKIP**。
  恢复轮询路径未构造真实 30 秒卡死端到端样本，按代码链与 v7 门禁验证。
- 结论：已修复。

#### C4-24 缩略图解码移出 GUI 线程 — 已修复
- 改动：`FileWrapper/mainwindow.{h,cpp}` 新增单线程 `QThreadPool`、
  `ThumbnailGate`、`ThumbnailDecodeTask`、`onThumbnailDecoded()`、
  `waitForThumbnailReads()`、`prepareThumbnailsForArchiveWrite()`。
  后台按归档路径/偏移/长度快照自行开 `QFile` 读取并校验 CRC，解码/缩放/合成
  `QImage`；GUI 只做 `QPixmap::fromImage()` 与 `setIcon()`。
  保存前取消并等待在途读取（最多 5 秒），避免后台归档句柄让 rename 失败。
- 验证：H21a-d 全部 PASS；运行期“解码任务在 GUI 线程”自检告警 **0 次**；
  线程化后全量回归 **245 PASS / 0 FAIL / 0 SKIP**（本次检测到音频设备，
  V13b/c 两条无设备条件用例未执行；官方 runner `-SkipShots` 为 242 PASS/3 SKIP）。
- 结论：已修复。

## 三、新增/扩展回归测试

| 用例 | 覆盖 | 结果 |
|---|---|---|
| A4 扩展 | 活 PID + 40 秒旧锁仍拒绝；死 PID + 40 秒旧锁可清理 | PASS |
| A11 扩展 | ZipReader 拒绝“声明 0 字节却带数据”的条目，预算不被记 0 | PASS |
| A12 新增 | 单元素 5000 属性被 `xmlWithinTagBudget` 拒绝；正常 XML 通过 | PASS |
| H21a-d（已有） | 缩略图视图/解码/角标/切回树形 | PASS |
| V3、P 组（已有） | EPUB、FWDA v3 元数据/导出恢复/兼容 | PASS |

本轮测试总数增至 245 项（有音频设备；无设备 247）。新增 A12、V15a–c、V6c
共 5 条断言，A4/A11 为扩展同一断言。修复前 238 PASS/4 SKIP；最终
`-SkipShots` 为 242 PASS/3 SKIP，`--shots` 为 245 PASS/0 SKIP。

## 四、行为变化与风险

1. **缩略图**：每次重建最多为前 1024 张生成缩略图，其余保留类型角标；状态栏会说明。
   这是用“可见性换内存上限”的取舍，图片特别多时用户需用筛选查看后续文件。
2. **插件 JSON**：改 JSON 后必须清理该插件的 moc/对象；只重跑 qmake 仍不会更新。
   文档已改，构建系统未做自动依赖。
3. **PDF**：损坏/加密 PDF 现在返回失败并显示原因，不再显示“空文档”。
4. **RTF**：达到上限时会在正文末尾显示截断提示，`truncated` 不再固定 false。
5. **XML 预算**：单元素属性 >4096 的合法但极端文档会被拒绝（显示截断/解析失败），
   阈值在 `xmlguard.h` 可调。
6. **package.ps1**：`OutDir` 不允许在仓库内，原先把产物打到 `repo\dist` 的用法会被拒绝；
   需改到仓库外。这是 P1 安全修复的必要限制。
7. **写锁**：活进程的长保存不会被第二个实例抢锁；崩溃残留锁仍需 30 秒后才清理，
   清理前还会检查 PID。

#### C4-25 OPC 百分号解码仓库内回归 — 已修复
- 改动：`tests/fixtures/make_fixtures.py` 新增 `make_opc_percent_docx/pptx/xlsx`，
  生成 `opc_percent.docx|pptx|xlsx`；`tests/plugin_tests.cpp` 新增 V15a–c。
- 覆盖：三个包分别把主文档、幻灯片、工作表部件名写成 `%20`，ZIP 条目名是
  字面空格；断言三个插件都能读出对应正文。
- 验证：V15a–c 全部 PASS；生成器可重复生成三个 fixture。
- 结论：已修复。

#### C4-26 测试 runner 超时 — 已修复
- 改动：`tests/run-plugin-tests.ps1` 新增 `-TimeoutSec`（默认 600），用
  `System.Diagnostics.ProcessStartInfo` + `WaitForExit(ms)`；超时后
  `taskkill /PID /T /F` 并返回 **124**。避开 PowerShell 5.1
  `Start-Process -NoNewWindow` 下 `ExitCode` 为空的问题。
- 验证：`-TimeoutSec 1` 实测 `TIMEOUT after 1s; killing process tree`、
  `RESULT: TIMEOUT`、`exit : 124`；正常 `-TimeoutSec 300` 实测 `exit : 0`。
- 结论：已修复。

#### C4-27 ODT 表格 repeat — 已修复
- 改动：`Plugins/PluginWord/worddocument.cpp:2284-2345` 读取
  `table:number-rows-repeated` / `table:number-columns-repeated`，按
  `kMaxOdtRepeatPreview=40` 物化；超限 `markTruncated`；逐行 `accountHtml`。
- 验证：新增 `sample_odt_repeat.odt`（2 行 × 3 个 R + 1 个 Z）与 V6c；
  V6a–c PASS。
- 结论：已修复。

## 五、未实证项（不计入 27 项已修复）

| 项 | 原因 | 建议 |
|---|---|---|
| 音频设备热拔 | 未实测，不能确认 QAudioSink 状态序列 | 配真机拔插用例后再改 |

## 六、涉及文件

主程序：`FileWrapper/{mainwindow.cpp,mainwindow.h,filemanager.cpp}`、
`FileWrapper/deploy/package.ps1`、`include/plugininterface.h`、`README.md`。

插件：`Plugins/PluginBook/{mainwindow.cpp,mainwindow.h}`、
`Plugins/PluginMedia/{mainwindow.cpp,mainwindow.h}`、
`Plugins/PluginPdf/{mainwindow.cpp,mainwindow.h,pluginpdf.cpp,pluginpdf.h}`、
`Plugins/PluginSheet/sheetdocument.cpp`、`Plugins/PluginSlides/slidesdocument.cpp`、
`Plugins/PluginWord/{rtfdocument.cpp,rtfdocument.h,worddocument.cpp}`、
`Plugins/common/{xmlguard.h,zipreader.cpp,zipreader.h}`。

测试/文档：`tests/plugin_tests.cpp`、`tests/README.md`、`tests/run-plugin-tests.ps1`、
`Plugins/plugin_common.pri`、`AUDIT_2026-11-19_FIX_REPORT.md`、
`.workbuddy/memory/MEMORY.md`、本报告与主报告。

*本报告不包含 commit/push；所有验证命令均在 `_bw/audit4` 影子目录与独立子进程中执行。*

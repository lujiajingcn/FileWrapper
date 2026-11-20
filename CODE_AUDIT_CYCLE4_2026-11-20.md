# FileWrapper 第四轮全工程循环审计报告

- 审计日期：2026-11-20
- 审计基线：`HEAD = 6f67515`「feat(ui): 文件列表新增缩略图显示」（2026-11-19）
- 历史基线：`CODE_AUDIT_CYCLE3_2026-11-19.md`（16 项，HEAD=28dbbe5）。
  第三轮的 16 项已在 `8255612`、`ebb46f4` 修复；随后 `ab503f7` 落地另一份审查改进，
  `3bcd9e3` 新增 FWDA v3 文件系统元数据，`6f67515` 新增缩略图视图。本轮审计的是
  这些改动之后的完整代码树，不是重复核对第三轮清单。
- 审计范围：`FileWrapper/`、`include/`、`Plugins/`（8 个插件 + `Plugins/common/`）、
  `resources/`、`tests/`、`FileWrapper/deploy/`、顶层 qmake/脚本/文档。
- 排除：`Plugins/PluginBook/thirdparty/libmobi` 与 FFmpeg/qtpdfium 的 vendored 头/库；
  `_bw/`、`tests/build/`、`build/` 等生成物；工作区原有未跟踪目录 `_eval_merge_buttons/`
  （非本任务产生，未删除、未纳入结论）。
- 方法：先建立修复前基线，再循环审计。每轮按「主程序 / 解析查看器插件 / 媒体与线程 /
  构建部署测试文档」四维并行读完整调用链；候选逐条用源码交叉引用、独立探针、构造
  最小样本或只读布尔复算核实。第 2 轮对确认项修复；第 3 轮重读全部修复点并复查
  此前未重点检查的区域（FB2/EPUB 上限、QDom 属性放大、打包子目录删除、锁的进程存活、
  缩略图内存、PDF/RTF 失败路径）。第 3 轮未再发现新的确认问题，满足“修复后
  再审计一轮无新增”的收敛条件。

## 一、结论概览

本轮确认 **27 项**问题并全部修复：**P1 1 项、P2 9 项、P3 17 项**。
另有 **1 项**未实证（播放中音频设备热拔），在第五节列出，不把猜测写成确认问题。
C4-23（`m_setStopUnsafe` 永不复查）、C4-24（缩略图 GUI 线程解码）、C4-25（OPC
百分号 fixture）、C4-26（runner 超时）、C4-27（ODT repeat）在本轮收尾后按用户
要求继续完成，状态均从“残余”变为“已修复”。误报与排除候选见第六节。

| 区域 | 确认 | P1 | P2 | P3 | 已修复 |
|---|---:|---:|---:|---:|---:|
| 主程序 / 归档模型 | 9 | 1 | 3 | 5 | 9 |
| 解析 / 查看器插件 | 10 | 0 | 4 | 6 | 10 |
| 媒体 / 线程 / 生命周期 | 2 | 0 | 0 | 2 | 2 |
| 构建 / 部署 / 文档 / 测试 | 6 | 0 | 2 | 4 | 6 |
| 合计 | 27 | 1 | 9 | 17 | 27 |

严重性约定：**P1** 不可逆数据丢失、安全问题、构建/启动失败；**P2** 可触发的功能
错误或明显资源/数据风险；**P3** 边缘路径、生命周期清洁度、文档/测试一致性。

## 二、修复前基线（2026-11-20 01:30–01:40）

1. **完整构建**：在 `_bw/audit4` 影子构建目录执行
   `qmake ..\..\FileWrapper.pro -spec win32-g++ "CONFIG+=debug"` + `mingw32-make -j6`。
   结果 **0 error**；共 47 条编译警告，全部来自 vendored libmobi（`src/miniz.h`
   的 `MINIZ_NO_*` 重复定义、`src/miniz.c` 未使用函数、`src/write.c` 的
   `drm_offset` 未使用），**工程代码 0 警告**。`deploy-deps.ps1` 复制 23 个依赖，
   0 warning。
2. **回归套件**：本机沙箱的 `tests/build/` 被旧 ACL 的
   `Everyone: DeleteSubdirectoriesAndFiles=Deny` 锁住，qmake 无法替换已有
   `Makefile.Debug`，官方 runner 在该目录上失败（环境限制，不是测试代码缺陷）。
   为取得可信基线，用同一 `tests/plugin_tests.pro` 在 `_bw/audit4/testsuite`
   影子目录构建测试程序，并使用独立 `cmd.exe` 子进程运行（避免 PowerShell 5.1
   把 stderr 的 `[INFO]` 当终止错误）：
   `plugin_tests.exe <fixtures> <app-plugins> <log> --integrate`。
   修复前结果：**238 PASS / 0 FAIL / 4 SKIP，退出码 0**。
   4 个 SKIP = 3 个未启用 `--shots` 的截图断言 + D6（沙箱禁止启动子进程）。
   本机无音频输出设备，V13b/c 按无设备分支执行。
3. 这组数字是本轮修复后对比的基线；第三方警告与 SKIP 不计入回归。

## 三、确认问题总表

| 编号 | 级别 | 标题 | 位置 | 状态 |
|---|---|---|---|---|
| C4-01 | P1 | `package.ps1` 仍允许把 `OutDir` 指向仓库内子目录并递归删除 | `FileWrapper/deploy/package.ps1:60-66` | 已修复 |
| C4-02 | P2 | 缩略图缓存上限只限 QHash，模型项持有全部 QPixmap，内存线性增长 | `FileWrapper/mainwindow.cpp:79-89,3675-3685` | 已修复 |
| C4-03 | P2 | `.fwdalock` 仅按 30 秒 mtime 判陈旧，长保存被第二个实例抢锁 | `FileWrapper/filemanager.cpp:179-270` | 已修复 |
| C4-04 | P2 | FB2 正文递归无深度上限，1500 层嵌套即可栈溢出崩溃 | `Plugins/PluginBook/mainwindow.cpp:166,730,872` | 已修复 |
| C4-05 | P2 | `xmlWithinTagBudget` 只数 `<`，单元素海量属性可让 QDom 卡死 | `Plugins/common/xmlguard.h:15-93` | 已修复 |
| C4-06 | P2 | EPUB 解压次数上限 2048 < 条目上限 4096，后续章节静默丢失/误报损坏 | `Plugins/PluginBook/mainwindow.cpp:74-120,519-520,680-681` | 已修复 |
| C4-07 | P2 | ZIP 条目声明 0 字节却带压缩数据；预算记 0、仍可反复解压 | `Plugins/common/zipreader.cpp:260-350` | 已修复 |
| C4-08 | P2 | `package.ps1` 漏复制 `translations/`，打包版中文按钮失效并误报缺依赖 | `FileWrapper/deploy/package.ps1:87,105` | 已修复 |
| C4-09 | P2 | “改插件 JSON 后重跑 qmake 即可”不成立，内嵌元数据/sidecar 不更新 | `Plugins/plugin_common.pri:63-75` | 已修复（文档/约定） |
| C4-10 | P2 | 停播未确认时 `releaseMappedInput()` 仍 unmap，卡住的 worker 读到悬空内存 | `FileWrapper/mainwindow.cpp:2399-2415` | 已修复 |
| C4-11 | P3 | 插件窗口析构不先 `requestStop()`，暂停中析构 3 秒超时并保留线程 | `Plugins/PluginMedia/mainwindow.cpp:1438-1465` | 已修复 |
| C4-12 | P3 | `doSeek` 停等超时后残留 `m_ioInterrupt=1`，恢复播放立即被当 EOF | `Plugins/PluginMedia/mainwindow.cpp:1731-1751` | 已修复 |
| C4-13 | P3 | `QFile::map()` 失败时读插件旧 `lastError()`，误报原因并跳过临时文件回退 | `FileWrapper/mainwindow.cpp:2087-2100` | 已修复 |
| C4-14 | P3 | Slides/Sheet 单页/单表 XML 超预算或损坏时静默消失，无截断提示 | `Plugins/PluginSlides/slidesdocument.cpp:392-407`；`PluginSheet/sheetdocument.cpp:399-414` | 已修复 |
| C4-15 | P3 | 损坏/加密 PDF 显示“空文档”且内存通道返回成功，路径通道无原因 | `Plugins/PluginPdf/mainwindow.cpp:84-145`；`pluginpdf.cpp:5-42` | 已修复 |
| C4-16 | P3 | EPUB 仍用有损 `QUrl::fromPercentEncoding`，文件名带字面 `%` 整本打不开 | `Plugins/PluginBook/mainwindow.cpp:33-38` | 已修复 |
| C4-17 | P3 | “未保存改动”提示里保存后不刷新列表/偏移，取消加载对话框后状态失配 | `FileWrapper/mainwindow.cpp:2975-3007` | 已修复 |
| C4-18 | P3 | 扁平归档再次添加同一文件检测不到重复，直到保存才失败 | `FileWrapper/filemanager.cpp:926-953` | 已修复 |
| C4-19 | P3 | RTF 达到 5 万段/2000 万字符上限仍报 `truncated=false`，无提示 | `Plugins/PluginWord/rtfdocument.cpp:128-187`；`worddocument.cpp:1166-1177` | 已修复 |
| C4-20 | P3 | 测试基线/记忆文档漂移（README 236、MEMORY 194 与 237/238 实跑不符） | `tests/README.md:3-6`；`.workbuddy/memory/MEMORY.md` | 已修复 |
| C4-21 | P3 | 插件接口注释写“10 个虚函数”，实际 13 个（另有虚析构） | `include/plugininterface.h:19-22` | 已修复 |
| C4-22 | P3 | 根 README 只有 44 字节；测试 runner 文件内的中文注释违反 ASCII-only 约定 | `README.md`；`tests/run-plugin-tests.ps1:158` | 已修复 |
| C4-23 | P3 | `m_setStopUnsafe` 永不复查，一次停播超时导致保存/重载永久降级 | `FileWrapper/mainwindow.cpp:1860-1911` | 已修复（v7 非阻塞复查） |
| C4-24 | P3 | 缩略图读条目/解码在 GUI 线程，单张 40 MP 图可阻塞约 194 ms | `FileWrapper/mainwindow.cpp:3538-3607` | 已修复（QThreadPool） |
| C4-25 | P3 | OPC 百分号解码没有仓库内 docx/pptx/xlsx 回归样本 | `tests/fixtures/opc_percent.*`；`tests/plugin_tests.cpp` V15 | 已修复（3 个 fixture + 3 条断言） |
| C4-26 | P3 | 测试 runner 无超时，测试挂起会永久阻塞 | `tests/run-plugin-tests.ps1:171-205` | 已修复（TimeoutSec + 杀进程树） |
| C4-27 | P3 | ODT 表格忽略 number-rows/columns-repeated，列错位/重复行丢失 | `Plugins/PluginWord/worddocument.cpp:2284-2345` | 已修复 |

## 四、确认问题详情

### C4-01（P1）package.ps1 仍可删除仓库内子目录

- 位置：`FileWrapper/deploy/package.ps1:55-73`（守卫与删除）。
- 触发：`-OutDir <repo>\.git`、`<repo>\FileWrapper` 或任意仓库子目录，并带
  `-Overwrite`（或该目录恰好有 `.filewrapper-package` 标记）。
- 证据：第三轮补了 `outFull == repoRoot` 和“OutDir 是 repoRoot 祖先”的判断，
  但没有“OutDir 是 repoRoot 后代”的判断。`<repo>\.git` 既不等仓库根，也不是其祖先，
  三个旧守卫全放行；随后的
  `Remove-Item -LiteralPath $OutDir -Recurse -Force` 会删除 `.git` 或源码目录。
  只读布尔复算：`repo\.git -> blocked=False`（修复前）。
- 影响：不可逆删除仓库元数据/源码；后果与第三轮 P1 相同，只是目标从仓库根变成子目录。
- 修复：增加**双向包含**判断——`OutDir` 等于仓库根、位于仓库根内、或是仓库根祖先
  一律拒绝；打包输出必须放仓库外。修复后复算 `.git`/`FileWrapper`/仓库根均 blocked。
- 状态：已修复。

### C4-02（P2）缩略图内存不设上限

- 位置：`FileWrapper/mainwindow.cpp:79-89`（尺寸/上限常量）、
  `:3498-3520`（全部图片入 pending）、`:3667-3690`（逐张生成、缓存满只 clear）。
- 触发：加载含大量图片条目的归档并切到缩略图视图。
- 证据：真实 HEAD 探针（1200 张 40×30 PNG 归档）：
  `rows=1200 decoded_thumbs=1200 rss_start=25760 KB rss_end=205740 KB delta=179980 KB`，
  每张约 149 KiB（=192×192×4）。缓存超过 1024 只执行
  `m_mapThumbnailIcons.clear()`，但每张图已经通过 `QStandardItem::setIcon()`
  被模型项持有；清 QHash 不释放像素。重建时 1024 张还要重新解码。
- 影响：5000 张图片约 720 MB，1 万张约 1.4 GB；相册/图片归档在缩略图模式下可 OOM。
- 修复：解码尺寸从 192 降到 128（96px 网格仍清晰，单张约 64 KiB）；新增
  `kThumbnailMaxGenerated=1024`，每次重建按“已缓存 + 已生成”计数，达到上限即停止
  并提示“只为前 1024 张生成缩略图，可用筛选查看其余文件”。峰值从线性无界降到约 64 MB。
- 状态：已修复（单张解码延迟已由 C4-24 的 QThreadPool 改造解决）。

### C4-03（P2）写锁的 30 秒陈旧判定会抢走活锁

- 位置：`FileWrapper/filemanager.cpp:179-270`。
- 触发：实例 A 保存/合并同一输出超过 30 秒（几 GB 归档即可），实例 B 同时保存。
- 证据：旧实现只读锁文件 mtime，超过 30 秒就 `QFile::remove()`；写入的 PID 全仓
  无人读取。锁探针：
  `A fresh-lock: lock=1 save_ok=0`；把 mtime 改到 40 秒前，
  `B stale-lock(40s): lock=1 save_ok=1 lock_after=0`。
- 影响：B 删掉 A 的锁后两个进程同时走“临时文件→备份→rename”，后 rename 者覆盖前者，
  前者成果静默丢失；交错回滚还可能留下半套备份。
- 修复：保留 30 秒作为“可能崩溃”的门槛，但删锁前必须用
  `isProcessAliveForLock()`（Windows `OpenProcess` + `GetExitCodeProcess`；Unix `/proc`）
  确认锁内 PID 已退出；打不开进程时保守视为仍存活。析构只删除 PID 等于自己的锁。
- 状态：已修复；新增 A4 回归（活 PID + 40 秒旧 mtime 仍拒绝；死 PID + 旧 mtime 可清理）。

### C4-04（P2）FB2 递归栈溢出

- 位置：`Plugins/PluginBook/mainwindow.cpp:166`（`kMaxXmlDepth`）、
  `:730`（根调用）、`:869-903`（`fb2ToHtml` 递归）。
- 触发：FB2 正文嵌套足够深（1500 层 `<section>`，文件仅 28.6 KB）。
- 证据：编译真实 `mainwindow.cpp` 的探针：1500 层时进程退出码
  `-1073741571`（0xC00000FD，`STATUS_STACK_OVERFLOW`）；gdb 栈为
  `MainWindow::fb2ToHtml` 自我递归。`xmlWithinTagBudget` 的标签总数上限远未触发，
  QDom 解析本身也能成功，崩溃点在递归转换。
- 影响：打开恶意/损坏 FB2 直接杀死主程序（插件与宿主同进程）。
- 修复：`fb2ToHtml(node, nDepth)`；深度 >200 时停止并 `markTruncated`
  （“FB2 结构过深，部分内容未显示”），与 Word/Sheet/Slides 的 200 层口径一致。
- 状态：已修复；探针 1500 层不再崩溃。

### C4-05（P2）XML 属性放大绕过标签预算

- 位置：`Plugins/common/xmlguard.h:15-93`。
- 触发：单个元素包含海量唯一属性（XML 合法），`<` 只有几个，旧预算只数 `<`。
- 证据：2.29 MB、20 万个属性的 XML：旧 `xmlWithinTagBudget` 返回 true；
  `QDomDocument::setContent` 用时 **66.7 秒**，RSS 增加约 102 MB。属性插入是
  二次方成本，64 MB 单条目可放大到小时级/GB 级，GUI 线程无取消入口。
- 影响：docx/xlsx/pptx/ods/odp/FB2 全部可达；打开几百 KB 文件即可让程序假死。
- 修复：线性扫描增加“单元素属性数”预算（引号外 `=`，4096/元素），并整体跳过
  注释/CDATA/处理指令，避免其中的引号破坏状态；保留 500k 标签总预算与函数签名。
  修复后同一样本 0 ms 拒绝；3000 属性的正常文档通过。
- 状态：已修复；新增 A12 回归。

### C4-06（P2）EPUB 解压次数上限导致章节静默丢失

- 位置：`Plugins/PluginBook/mainwindow.cpp:74-120`（展开/原因）、
  `:519-520`（EPUB）、`:680-681`（.fb2.zip）；`Plugins/common/zipreader.h:46-47`。
- 触发：EPUB 中央目录条目数 >2048（画册/多图电子书常见）。
- 证据：2052 条目 EPUB：第 1 章显示，第 2 章消失，标签仍为“1 / 1”，无截断提示；
  把 `META-INF/container.xml` 放到第 2048 条之后，界面报“解析电子书失败，
  文件可能已损坏”，真实原因是提取次数预算耗尽。
- 影响：合法电子书章节/图片静默丢失并被误判为文件损坏；用户无法自救。
- 修复：打开 Book 的两个 ZipReader 时把 `maxExtractions` 提升到
  `max(默认, entryCount()+16)`；`expandZipEntries` 区分“0 字节条目”和
  “预算拒绝”，后者返回 false 并回传静态原因，UI 标记截断。
- 状态：已修复；2052 条目样本从 1 章恢复为 2 章。

### C4-07（P2）ZipReader 对“声明 0 字节”的预算绕过

- 位置：`Plugins/common/zipreader.cpp:260-350`。
- 触发：中央目录声明 `uncompSize=0`，但压缩数据非空（伪造/损坏 ZIP）。
- 证据：构造 1 MiB 实际内容、声明 0 字节的 deflate 条目（compSize 约 1 MiB），
  用真实 `ZipReader` 探针连续 5 次 `entry("a.bin")`：
  `out=1048576 total=0 extractions=1..5`。旧实现只检查
  `e.uncompSize > 0` 时的长度，因此返回 1 MiB 内容而累计预算始终记 0；
  同一图片被 2048 次引用可反复解压，绕过 256 MiB 累计上限。
- 影响：拒绝服务（GUI 线程反复解压）与损坏归档被当成合法内容。
- 修复：`uncompSize==0` 时要求 `compSize==0`，否则直接报“条目长度声明异常”；
  解压只按声明长度分配一次，`Z_BUF_ERROR`/输出超出即拒绝，不再 6 次放大重试。
- 状态：已修复；新增 A11 扩展回归。

### C4-08（P2）打包漏掉 Qt 中文翻译

- 位置：`FileWrapper/deploy/package.ps1:87`（复制子目录列表）、`:105`
  （`windeployqt --no-translations`）；清单
  `FileWrapper/deploy/plugin-deps.manifest` 声明 `translations/qtbase_zh_CN.qm`。
- 触发：正常执行 `package.ps1`。
- 证据：部署清单 23 项中唯一未被 package 复制的是 `translations/qtbase_zh_CN.qm`；
  构建目录里 deploy-deps 已经复制了它。包内又复制了 `plugin-deploy-manifest.txt`，
  启动自检按 `isFile()` 判定，于是打包版会误报“缺少 1 个依赖文件”，标准按钮也回退英文。
- 影响：打包版中文化失效 + 插件诊断误报。
- 修复：复制子目录列表加入 `translations`；`main.cpp` 已优先从
  `<exe>/translations` 加载，无需改程序。
- 状态：已修复。

### C4-09（P2）插件 JSON 修改后的构建说明不成立

- 位置：`Plugins/plugin_common.pri:63-75`；同说法见
  `AUDIT_2026-11-19_FIX_REPORT.md:183` 与 `.workbuddy/memory/MEMORY.md`。
- 触发：只改 `pluginX.json`（改后缀或 apiVersion），执行 `qmake && mingw32-make`。
- 证据：生成的 `moc_plugintext.cpp` 依赖只有插件头和 Qt 头，没有 JSON；
  `PluginText.dll` 链接依赖也没有 JSON。JSON 只出现在 `DISTFILES`（`make dist` 用）
  和 POST_LINK 的 sidecar 复制（仅链接时执行）。DISTFILES 不是 moc 的构建依赖，
  因此内嵌元数据保持旧值，sidecar 也不刷新（没有重链接）。
- 影响：新增后缀/新 API 版本静默不生效，属“改了没反应”的隐蔽构建缺陷。
- 修复：修正 `plugin_common.pri`、实施报告与长期记忆：必须删除该插件的
  `moc_*.cpp`/对象或 `mingw32-make clean` 后再 qmake+make；本轮不改变构建系统结构，
  避免为单个元数据依赖引入自定义 moc 规则。
- 状态：已修复（约定/文档层面）；构建系统层面的自动依赖仍为后续改进项。

### C4-10（P2）停播未确认时仍 unmap 内存映射

- 位置：`FileWrapper/mainwindow.cpp:2399-2415`（`releaseMappedInput`），
  调用点包括 `:2168-2176`、`:2258-2270`。
- 触发：同一插件实例上一个 ≥128 MiB 文件走 mmap 播放，换文件/重开时
  `stopPlaybackAndWait()` 2s+30s 都未退出；打开失败分支仍调用
  `releaseMappedInput()`。
- 证据：`releaseAllMappedInputs()` 明确跳过 `m_setStopUnsafe`，但
  `releaseMappedInput()` 没有判断；`~MappedInput` 会立即
  `pFile->unmap(pData)`。卡住的 worker 仍通过 `m_memIO.data` 读取该映射。
- 影响：悬空内存读，可能访问违例/崩溃。
- 修复：`releaseMappedInput()` 内统一检查 `m_setStopUnsafe`，命中则保留映射并
  qWarning；与 `releaseAllMappedInputs()` 的 P0-3 不变量一致。
- 状态：已修复。

### C4-11（P3）插件窗口析构不自足

- 位置：`Plugins/PluginMedia/mainwindow.cpp:1438-1465`。
- 触发：`~MainWindow` 时 worker 正阻塞在暂停条件变量，且调用方没有先
  `stopPlaybackAndWait()`。
- 证据：探针 `pause_delete`：删除耗时 **3006 ms**，日志出现
  “解码线程未在 3s 内退出：保留 worker/线程/上下文”，随后对象析构但 worker
  仍持有 `m_audioMutex/byteBuf` 裸指针；一旦被唤醒即 UAF。
- 影响：删除卡 3 秒、线程/FFmpeg 上下文泄漏；极端路径 UAF。
- 修复：析构先 `requestStop()`（原子标志 + `wakeAll`，不阻塞）再等待；
  `m_audioMutex`/`byteBuf` 改为 `std::shared_ptr`，worker 持有所有权，
  “保留 worker”分支不再留下悬空成员。
- 状态：已修复；暂停中删除探针不再触发 3 秒保留分支。

### C4-12（P3）seek 超时后残留中断标志

- 位置：`Plugins/PluginMedia/mainwindow.cpp:1731-1751`。
- 触发：拖进度条时 `stopWorkerAndWait(2000)` 超时（worker 正在长解码/慢 I/O）。
- 证据：`stopWorkerAndWait()` 先置 `m_ioInterrupt=1`；旧失败分支恢复播放状态，
  但没有复位中断标志，worker 下一次 `av_read_frame` 得到 `AVERROR_EXIT`，
  被当作 EOF 走 drain 并结束播放。状态栏“放弃本次跳转”无法兑现。
- 影响：seek 超时后播放器自行停止。
- 修复：失败分支恢复前 `m_ioInterrupt.store(0)`；只有
  `waitForDecodingStopped(0)` 确认 worker 已退出才清停止请求，避免清掉仍在运行
  worker 的停止请求。
- 状态：已修复。

### C4-13（P3）map 失败时读旧 lastError

- 位置：`FileWrapper/mainwindow.cpp:2087-2100`。
- 触发：先打开一个坏媒体（lastError 非空），再打开 ≥128 MiB 条目且
  `QFile::map()` 失败（归档被外部改写/地址空间不足）。
- 证据：旧代码
  `bMappedAccepted = (pMapped != nullptr && iface->sendFileMapped(...))`，
  然后无条件 `iface->lastError()`；`pMapped==nullptr` 时插件根本没被调用，
  读到的是上一个文件的原因，并据此跳过临时文件回退。
- 影响：把上一次失败原因报给这个文件，且放弃本可成功的路径回退。
- 修复：只在 `pMapped != nullptr` 且插件返回 false 时读取 `lastError()`；
  map 失败直接走临时文件通道。
- 状态：已修复。

### C4-14（P3）Slides/Sheet 单部件失败静默丢页/丢表

- 位置：`Plugins/PluginSlides/slidesdocument.cpp:392-407`；
  `Plugins/PluginSheet/sheetdocument.cpp:399-414`。
- 触发：某一张 slide/worksheet 的 XML 超 50 万标签或损坏，其余部件正常。
- 证据：2 页 pptx 第 2 页 XML 超预算：`open=true slideCount=1 truncated=''`，
  HTML 显示“第 1 页 / 共 1 页”，无截断提示；2 表 xlsx 只显示 1 表，同样无提示。
- 影响：用户以为文档只有一页/一个表，静默丢内容。
- 修复：两条 `continue` 前按页/表 `markTruncated` 并写清原因；全部页/表都失败时
  `open()` 返回 false，文案附带首个截断原因。
- 状态：已修复。

### C4-15（P3）损坏/加密 PDF 显示“空文档”

- 位置：`Plugins/PluginPdf/mainwindow.cpp:84-145`；`pluginpdf.cpp:5-42`。
- 触发：损坏/加密/不支持的 PDF，且文件 <128 MiB（普通 PDF 走内存通道）。
- 证据：`showPdf(garbage)` 后界面 `labels=[空文档]`、无错误文案，插件返回 true，
  宿主不弹框；`QPdfDocument::error()` 只进 qWarning。路径通道返回 false 但
  `lastError()` 为空。
- 影响：用户把“PDF 损坏/加密”理解成“空文件”，加密 PDF 无任何提示。
- 修复：新增 `pdfErrorText()`（InvalidFileFormat/IncorrectPassword/
  UnsupportedSecurityScheme/FileNotFound/DataNotYetAvailable → 中文）；
  `showPdf` 改返回 bool，失败写内容区并实现 `lastError()`/`clearLastError()`；
  `pageCount()<=0` 返回 false。
- 状态：已修复。

### C4-16（P3）EPUB 裸 `%` 被有损解码

- 位置：`Plugins/PluginBook/mainwindow.cpp:33-38`。
- 触发：EPUB 条目名含字面 `%`（如 `ch3%.xhtml`、`100%.png`）。
- 证据：`QUrl::fromPercentEncoding("100%.png")` 得到 `"100\uFFFDng"`（吃掉 `%.p`）；
  `percentDecodeStrict` 得到原值。单章 EPUB 对照：`%20`/字面空格正常，
  裸 `%` 章节丢失并报“文件可能已损坏”。
- 影响：合法 EPUB 从可显示退化为整本报损坏。
- 修复：Book 的 `joinPath` 改用 `Plugins/common/percentdecode.h` 的
  `percentDecodeStrict`，与三个 OOXML 插件统一。
- 状态：已修复。

### C4-17（P3）确认保存后未刷新界面/偏移

- 位置：`FileWrapper/mainwindow.cpp:2975-3007`（`confirmDiscardOrSaveIfDirty`）、
  `:4449-4454`（Ctrl+S 的刷新逻辑）。
- 触发：有未保存改动 → 点“加载文件” → 提示里选“保存” → 保存成功后取消文件对话框。
- 证据：`FileManager::save()` 重写归档后会重算 displayPath 并清 pending
  （`filemanager.cpp:2097`、`:846`）；列表模型里仍是保存前的路径/偏移快照。
  旧 `confirmDiscardOrSaveIfDirty()` 保存成功后不做
  `invalidateThumbnailCache/refreshTabInfosFromManager/refreshFilePathView/syncActionStates`。
  对 v1/跨盘归档，`E:/x -> E/x` 后双击会报“归档内未找到文件”；≥128 MiB 条目
  还会以 `nContentPositon=0` 去 mmap。
- 影响：列表与 FileManager 失配，操作失败/可能映射错误内容，保存按钮和标题 `*` 也残留。
- 修复：抽出 `afterArchiveSaved()`，Ctrl+S 与确认保存共用。
- 状态：已修复。

### C4-18（P3）扁平归档重复添加检测失效

- 位置：`FileWrapper/filemanager.cpp:926-953`。
- 触发：打开 root 为空的扁平归档（跨盘合并产生），再次添加归档里已有的文件。
- 证据：`addFiles` 只用 `m_mapFileInfos.contains(fp)` 比较源绝对路径，而扁平归档
  条目 `sFilePath` 是 `E/x` 形式；保存时 `computeStoredPaths` 才把 `E:/x` 压成
  `E/x` 并报重复。用户会看到两条，保存被阻断。
- 影响：保存失败，需要手工删除“看起来路径不同”的重复项（可恢复，不丢数据）。
- 修复：添加前用与保存一致的规范化键（`absoluteFilePath` 归一化、盘符冒号转首段、
  小写）建立现有条目集合，命中即跳过。
- 状态：已修复。

### C4-19（P3）RTF 截断不记账

- 位置：`Plugins/PluginWord/rtfdocument.cpp:128-187`；
  `Plugins/PluginWord/worddocument.cpp:1166-1177`。
- 触发：RTF 超过 5 万段或 2000 万字符。
- 证据：60000 段 RTF 探针：`truncated=false`、`paragraphs=50000`、P59999 缺失、
  无任何截断提示；旧代码在 RTF 分支固定 `rtfRes.truncated=false`。
- 影响：用户看到半篇文档却以为完整。
- 修复：`RtfDocument` 增加 `m_truncated`/`isTruncated()`，在段落/字符上限处置位；
  Word 渲染时把 `truncated` 传给 `RenderResult`，并在 HTML 末尾追加
  “RTF 内容超过预览上限，仅显示前一部分内容”。
- 状态：已修复。

### C4-20（P3）测试/记忆基线漂移

- 位置：`tests/README.md:3-6`；`.workbuddy/memory/MEMORY.md:54,208-211,247`。
- 触发：按文档核对基线或按记忆判断已知问题。
- 证据：README 写 236/233+3，实际 HEAD 已有 H21 缩略图四条、P 组 v3 等；
  MEMORY 仍写 194 PASS、仍称 `package.ps1` 缺 `OutDir==repoRoot` 判断（该判断已在
  `8255612` 加入）。本轮实跑 239 PASS/4 SKIP（新增 A12 后）。
- 影响：回归基线误判；记忆把已修 P1 说成未修，易重复排查。
- 修复：README 改为 247 项（无设备；有设备 245）、本机 245 PASS+0 SKIP /
  242 PASS+3 SKIP 并说明 SKIP 来源；MEMORY 同步基线、打包守卫、锁语义、
  JSON 构建要求、ZipReader/缩略图约束。
- 状态：已修复。

### C4-21（P3）插件接口虚函数数量注释错误

- 位置：`include/plugininterface.h:19-22`。
- 证据：注释写“现在 10 个虚函数”，实际有 13 个虚函数（3 个纯虚 + 10 个有默认实现）
  外加虚析构。
- 修复：注释改为“13 个虚函数（另有虚析构）”。仅改注释，不改变 ABI，不升
  `PLUGIN_API_VERSION`（仍为 6，8 个插件 JSON 仍为 6）。
- 状态：已修复。

### C4-22（P3）根 README 与 runner ASCII 约定

- 位置：`README.md`；`tests/run-plugin-tests.ps1:158`。
- 证据：根 README 只有标题和一句介绍，没有构建/测试/打包入口；
  runner 头部声明 ASCII-only，但第 158 行有中文注释（当前未吞换行，属潜在风险）。
- 修复：README 补充前置、影子构建、`deploydeps`、测试、打包与插件 JSON 清理规则；
  runner 该行改成英文注释。
- 状态：已修复。

### C4-23（P3）停播超时后永久降级

- 位置：`FileWrapper/mainwindow.cpp:1860-1911`（`ensureInstanceStopped` /
  `ensureAllInstancesStopped`）、保存入口 `:3074-3081` / `:4807-4814`、
  重载入口 `:1600-1607`。
- 触发：PluginMedia 的 `stopPlaybackAndWait()` 先等 2 秒、再等 30 秒仍无法确认
  `doDecode` 退出（网络盘/驱动/解码器卡死），宿主把实例插入 `m_setStopUnsafe`。
- 证据：`m_setStopUnsafe` 全仓只有 `insert`/`contains`，没有 remove/clear；
  `ensureInstanceStopped()` 对已在集合里的实例直接 `return false`，因此
  `m_vtDeferredInstances` 的“再试一次”循环永远走不到成功分支。后果是保存/重载
  永久被拒、映射与临时文件保留、`PluginManager::setUnloadEnabled(false)` 不再恢复，
  即使后台线程几秒后已经退出。
- 修复（接口 v7）：
  - `PluginInterface` 新增 `isPlaybackStopped()`（非阻塞、const）。默认实现保守地
    返回 `!hasBackgroundPlayback()`；PluginMedia 用 `VideoWorker::isDecodingActive()`
    原子查询。
  - `MainWindow` 新增 `m_stopUnsafePoll`（1 秒）与 `pollStopUnsafeInstances()`：
    对 unsafe 集合逐个查询；为 true 的实例移出集合、释放映射；安全后销毁延期实例，
    全部清空后恢复 `PluginManager::setUnloadEnabled(true)` 并状态栏提示。
  - 保存/重载入口先主动调用一次 poll，避免“后台已退出但界面还没到下一秒”的误拒。
  - 轮询只查原子状态，不再次调用 `stopPlaybackAndWait()`，不会把 GUI 再阻塞 30 秒。
  - `PLUGIN_API_VERSION` 6→7，8 个插件 JSON 同步 `apiVersion:7`。
- 验证：全量构建 0 error；W6 `8 sidecar(s), all apiVersion=7`、W8a
  `apiVersion=7 accepted`、W1 全部插件正常加载；回归 245 PASS/0 FAIL/0 SKIP。
  未构造真实 30 秒卡死线程的端到端样本，恢复路径按代码链与 v7 门禁验证。
- 状态：已修复。

### C4-24（P3）缩略图解码移出 GUI 线程

- 位置：`FileWrapper/mainwindow.cpp:3538-3607`（同步读+解码）、
  `:3622-3665`（空闲回调调度）。
- 触发：缩略图视图下生成图片预览，同时用户滚动/操作或另一标签页播放媒体。
- 证据：同一 40 MP 图片，`entryContent` 读取+CRC 约 58 ms，
  `QImageReader::read`+缩放约 194 ms，全部在 GUI 线程串行执行；播放中会阻塞
  Media 的 20 ms 定时器（音频写出/进度/画面刷新）。
- 影响：单张约 0.2 秒界面冻结；大量图片时事件循环大部分时间用于解码，媒体可欠载。
- 修复：
  - 新增 `QThreadPool`（单线程，`kThumbnailMaxInFlight=1`）和
    `ThumbnailDecodeTask`；后台按“归档路径+偏移+长度”快照自行开 `QFile` 读取，
    校验 GUI 线程快照的 CRC，再解码/缩放/合成 `QImage`。
  - GUI 线程只做 `QPixmap::fromImage()` 和 `QStandardItem::setIcon()`；
    `QPixmap` 不在后台线程创建。
  - 代数 `m_nThumbnailEpoch` + `ThumbnailGate::generation` 作废旧结果；
    完成回调经 `qApp` 投递，`QPointer<MainWindow>` 校验窗口存活。
  - 保存/替换归档前 `prepareThumbnailsForArchiveWrite()` 取消并等待后台读取结束
    （最多 5 秒），避免 Windows 下后台归档句柄让 rename 失败；超时则明确中止保存。
  - 保留 `renderEntryThumbnail()` 同步版本给测试直接调用；生产路径走后台任务。
- 验证：H21a-d 全部 PASS；新增运行期自检，测试日志中
  “缩略图解码任务意外运行在 GUI 线程”出现 **0 次**；线程化后全量回归
  **245 PASS / 0 FAIL / 0 SKIP**（本次运行检测到音频设备，V13b/c 两条无设备
  条件用例未执行；官方 runner `-SkipShots` 为 242 PASS/3 SKIP）。
- 状态：已修复。

### C4-25（P3）OPC 百分号解码的仓库内回归

- 位置：`tests/fixtures/opc_percent.docx|pptx|xlsx`；
  `tests/plugin_tests.cpp` 的 V15a–V15c；生成器
  `tests/fixtures/make_fixtures.py` 的 `make_opc_percent_*`。
- 触发/缺口：三个插件的 OPC `Target` 解码已修，但此前只有 gitignore 的临时探针；
  未来移除 `percentDecodeStrict()` 或改回有损 `QUrl::fromPercentEncoding()`，
  所有现有测试仍会全绿。
- 修复：新增三个最小 OPC 包，把主文档/幻灯片/工作表部件名分别写成
  `word/document%20main.xml`、`slides/slide%201.xml`、`worksheets/sheet%201.xml`，
  ZIP 条目名使用字面空格；V15a–c 分别断言三段正文可读出。生成器同步添加
  `make_opc_percent_*`，可重复生成。
- 验证：V15a–V15c 全部 PASS；若把解码调用去掉，三个断言会分别失败（部件查不到）。
- 状态：已修复。

### C4-26（P3）测试 runner 无超时

- 位置：`tests/run-plugin-tests.ps1:17-27`（`-TimeoutSec`）、`:171-210`（运行/杀进程）。
- 触发：测试程序死锁、模态框或解码调用长时间阻塞。
- 证据：旧实现 `& $TestExe @testArgs` 同步等待，无超时；进程挂起时 runner 永久阻塞，
  不打印 RESULT，CI 无法收尾。
- 修复：改用 `System.Diagnostics.ProcessStartInfo` + `WaitForExit(ms)`（PowerShell 5.1
  下 Start-Process + 重定向的 ExitCode 可能为空）；超时后 `taskkill /PID /T /F`
  杀进程树，返回 **124**，并打印 `RESULT: TIMEOUT`。默认 600 秒，可传
  `-TimeoutSec`。
- 验证：用 `-TimeoutSec 1` 实测得到 `TIMEOUT after 1s; killing process tree`、
  `exit : 124`；正常 `-TimeoutSec 300` 得到 `exit : 0` 和 `RESULT: 242 PASS,
  0 FAIL, 3 SKIP`（截图关闭）。
- 状态：已修复。

### C4-27（P3）ODT 表格 repeat

- 位置：`Plugins/PluginWord/worddocument.cpp:2284-2345`。
- 触发：ODT 的 `table:number-columns-repeated` / `table:number-rows-repeated`，
  例如一个 `number-columns-repeated="3"` 的 `R` 单元格后跟 `Z`，应为 `R R R Z`。
- 证据：旧实现每个 XML 单元格只输出一个 `<td>`、每行只输出一个 `<tr>`；上述样本
  渲染成 `R Z`，后续列整体左移；重复空行/空列（常见 16384）直接消失。
- 修复：按原始 repeat 读取，物化时裁剪到 `kMaxOdtRepeatPreview=40`，超限
  `markTruncated`；行/列都复制；每个物化行都过 `accountHtml()`，避免 repeat
  绕过 HTML 预算；`covered-table-cell` 仍按 colspan 语义跳过。
- 验证：新增 `sample_odt_repeat.odt`（2 行 × 3 个 R + 1 个 Z）与 V6c；V6a–c 全部
  PASS。生成器同步添加 `make_odt_repeat()`。
- 状态：已修复。

## 五、未实证项（不计入确认问题）

1. **播放中音频设备热拔**：PluginMedia 没有连接 `QAudioSink::stateChanged/error`，
   设备在播放中被禁用/拔出时可能静默停写。本机 `defaultAudioOutput()` 不稳定，
   无法构造可靠的设备移除时序，因此不列为确认问题，也没有在本轮修改。
   后续应配真机拔插用例，再决定是否自动重建音频输出。

## 六、误报 / 非问题（排除理由）

| 候选 | 排除理由 |
|---|---|
| 缩略图排序后 `QPersistentModelIndex` 失效 | `populateFilePathThumbnails` 在 `applyCurrentSortToModel()` 之后重建映射；每次重建先 `cancelThumbnailGeneration()`；缩略图模式下没有别的排序触发点。 |
| 缩略图缓存失效遗漏 | 加载/添加/删除/保存/卸载/重载均在 `mainwindow.cpp:3058/4245/4313/4451/4565` 附近调用 `invalidateThumbnailCache()`；导出/校验不改归档内容；加载拒绝重复 displayPath。 |
| `QImageReader::size()` 无效时的解压炸弹 | 内置 PNG/JPEG/BMP/GIF/WebP/TIFF/ICO/XPM/SVG handler 均能先报尺寸；无效时仍受 32 MB 条目上限约束，且随后只做 `scaled()`；无实测炸弹，列残余风险。 |
| 媒体 `sws` 重建四条路径不一致 | 复用条件已含源宽高/像素格式；只有重建成功才提交 srcW/srcH/m_srcPixFmt，失败 `continue`；`av_image_get_buffer_size(RGB32,w,h,1)` 与 `sws_scale` 目标一致。 |
| `requestOutputSize` 打包/解包错位 | `h<<32|w` 与两处解包 `w=low32/h=high32` 对称，宽高成对。 |
| CRC 后台校验生命周期（C3-01/02 回归） | 校验线程自开 `QFile`、不捕获映射；`closeAllTabs` 递增 `m_nArchiveEpoch` 并清空两个集合；保存前 `waitForMappedCrc` 取消并等待句柄关闭；未发现新回归。 |
| v3 头部/CRC/兼容 | `ENTRY_FIXED_V3=52` 与读写两端一致；HeaderSize/FileCount/Version 在 CRC 覆盖内；v1/v2/v3 解析、v2 墓碑、时间戳收敛均有 P 组测试；未发现新问题。 |
| 三个 OOXML 插件路径穿越 | `percentDecodeStrict` 后再按段消解 `..` 且根目录钳制（`worddocument.cpp:441-460`、`slidesdocument.cpp:59-74`、`sheetdocument.cpp:372-390`）；`%2e%2e`/`%2f` 不会越界。 |
| EPUB 图片百分号解码 / 任意读盘 | `BookBrowser::loadResource` 对 decoded/encoded 两种 QUrl 均命中；只查 `m_entries`/`m_fb2Images`，末尾显式返回空，未发现 `file:///` 任意读。 |
| ZipReader stored/deflate 常规预算 | 两者都过 `chargeBudget`，stored 校验 CRC 与长度；仅“声明 0 字节”例外已由 C4-07 修复。 |
| 部署清单完整性 | `_bw/audit4` 构建日志 `[deploy] copied 23`、`warnings 0`；objdump 导入表与清单一致（Media 只需 av*/sw*/Qt6Multimedia；Qt6Pdf/Media 需要 Qt6Network；qsvg 需要 Qt6Svg）。 |
| 插件 API 版本一致性 | `PLUGIN_API_VERSION=6`，8 个插件 JSON 全为 6；加载门禁按精确值比较；本轮只改注释，未升版本。 |
| runner “退出码 1 且无 RESULT” | 历史误报：`powershell ... 2>&1 | Out-File` 会把测试程序 stderr 的 `[INFO]` 当终止错误；用独立子进程运行得到退出码 0。本轮继续用独立 `cmd.exe` 子进程。 |
| package.ps1 仓库根/祖先/盘根/大小写/`..` | 修复后均被 `EQ_REPO`/`INSIDE_REPO`/`DRIVE_ROOT`/祖先检查拦截；UNC/junction 的删除后果未实际执行，列为风险未实证。 |
| 顶层 `deploydeps` 默认配置 | 本机仅验证 `CONFIG+=debug`/Makefile.Debug 路径；默认 `debug_and_release` 选择未实测，不列为确认缺陷。 |

## 七、收敛依据与限制

1. 第 1 轮四维扫描发现的候选均在第 2 轮逐条核实；不能给出触发条件/证据的风格偏好
   与猜测被放入第六节，而不是计入问题数。
2. 第 2 轮修复后，第 3 轮重读全部改动点，并覆盖此前未重点检查的
   FB2/EPUB 上限、QDom 属性放大、package 子目录、锁的进程存活、缩略图内存、
   PDF/RTF 失败路径；**未发现新的确认问题**。
3. 完整构建：0 error；工程代码 0 warning（47 条警告全部来自 vendored libmobi）。
4. 回归：官方 runner 现在可直接运行（`tests/build` 的旧 ACL 已清理）：
   `-SkipShots` 为 **242 PASS / 0 FAIL / 3 SKIP**（3 条截图断言）；
   `--shots` 为 **245 PASS / 0 FAIL / 0 SKIP**。
   与修复前 238 PASS/0 FAIL/4 SKIP 相比无回归失败。
5. 未覆盖/限制：`package.ps1` 的 junction/UNC 删除未实际执行（只读布尔复算）；
   播放中拔音频设备未实测；C4-23 的恢复轮询未构造真实 30 秒卡死样本。

*本报告基于 `HEAD=6f67515` + 本轮工作区改动；不包含任何 commit/push。*

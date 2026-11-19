# FileWrapper 第二轮全工程循环审计（Round 1–13 + 真实性复核）

- 审计基线：`HEAD = ba64f13`（2026-11-18），工作区除既有未跟踪审计报告外无改动
- 审计方式：13 轮 × 每轮 3 个区域（主程序 / 插件 / 构建·部署·测试）并行静态审查；候选逐条读完整上下文，关键项用独立探针复现；每轮排除已知项后找“新问题”
- 停止条件：用户要求停止继续扫描；Round 13 后冻结，随后对 110 项候选做独立真实性复核
- 重要说明：**严格意义的收敛（某一轮完全没有新问题）没有达到**——Round 10 仍确认 6 项新问题（主程序 2、插件 2、构建 2）。本文件是“已确认问题”的冻结清单，不是“无缺陷证明”。

## 结论概览

去重、剔除同源/误报后，经过独立真实性复核后，最终确认 **109 项**问题（P1 1 项、P2 41 项、P3 67 项）：

| 区域 | 数量 | P1 | P2 | P3 |
|---|---:|---:|---:|---:|
| 主程序 | 34 | 1 | 9 | 24 |
| PluginMedia | 6 | 0 | 1 | 5 |
| 文档/查看器插件 | 40 | 0 | 25 | 15 |
| 构建/部署/测试 | 29 | 0 | 6 | 23 |

原附录中的 PDF 超大页面候选已实测确认，作为 D-40 计入。

验证标记：

- **实测**：用独立探针/真实插件/真实脚本复现过
- **源码**：完整代码路径与触发条件确认，未做运行期峰值测量
- **日志/产物**：仓库现有日志或构建产物直接证明

---

## 真实性复核与修正（独立复核）

在 13 轮审计后，另起 4 名独立复核员按区域逐条核对原始候选（不是重新找问题）；结合仓库探针与 Qt 6.9 源码复核，修正如下：

- **剔除误报 2 项**：M-01（Stop 后播放按钮死控件）、M-03（Stop 后 seek 只 resume 不重启音频）均基于 `de782b2` 修复前的旧行号，HEAD 已有 `doSeek(0)` 重播与 `ensureAudioStarted()`；实测 `AFTER_STOP=▶ → AFTER_CLICK=⏸`，从确认表移除。
- **新增确认 1 项**：原附录 X-01（PDF 超大页面尺寸）已用真实 PluginPdf + 433 字节 PDF 实测：12000×12000pt 页渲染出 12000×12000 图，QImage+Pixmap 约 1.1GB，列为 D-40。
- **严重性调整**：A-22 由 P2 升 P1（资源管理器同盘/Shift 拖放默认为 Move，接受但未搬运，源文件丢失）；A-13/A-14/A-16/A-30、T-04/T-17、M-05、D-14、D-36 由 P2 降 P3。
- **证据修正**：A-07 为 `list.at(i)`（非 at(0)）；A-16 删除“唯一无空间预检”表述（导出同样无预检）；A-26 的 GB 级措辞改为“宿主有 bad_alloc 捕获、风险在插件侧复制”；T-02 删除“RESULT 与 README 不一致”的错误结论；T-08/T-11/T-13/T-18 按实测修正（纯 Release 显式报错、未知 kind 实际 exit 1、SHELL 不由 PATH 决定、`zzz` 来自启动 sidecar）；D-25 限定为“纯文本/无自身格式 span”；D-33 删除未证实的“尾部伪造签名误判”；D-36 改为“显示尺寸无上限/布局失真”，不再称内存 DoS；D-37 明确“非 CP1252 分支每次调用无条件越界 1 字节”；T-28 的 exit 0 分支未复现，已注明。
- **复核结论**：109 项中 93 项为“确认”，16 项为“部分确认并已改写描述”；无剩余完全误报（原 M-01/M-03 已剔除）。严格“某一轮零新问题”仍未达到（Round 13 仍有 T-29），故本文档是“已确认问题冻结清单”，不是“无缺陷证明”。

## 修复进度（滚动更新）

| 批次 | 提交 | 已修 ID | 说明 |
|---|---|---|---|
| Batch 1 | `825f3a9` | A-01、A-02、A-06、A-07、A-13、A-21、A-22、A-23、A-25、A-26、A-34、T-28 | P1 拖放丢失源文件、合并目录/已加载归档、导出路径、CRC、tab 偏移、FindFile、dock、大文件、打包删除保护；新增 K6–K8 回归 |
| Batch 2 | `5569738` | A-33、D-32、D-36、D-37、T-23 | MOBI 书名越界、EXIF 方向、Slides 显示尺寸、RTF 断言、Unknown error 文案 |
| Batch 3 | `0c2a5e7` | A-15、D-14、D-30、T-11 | 自动保存重入守卫、RTF field 结果、MOBI CSS flow 过滤、部署清单坏行改 Fail |
| Batch 4 | `b65d9e6` | D-01、D-02、D-06、D-07、D-28 | XML 元素上限、container.xml 限量、Book 禁止本地 file:///、Sheet 实际格子/合并上限 |
| Batch 5 | `1eddbb3` | D-19、D-21、D-34、D-40 | gridSpan 钳制、Word 累计图片预算、Slides 内层块预算、PDF 渲染尺寸上限 |
| Batch 6 | `892ac96` | D-20、D-22、D-23 | Word 图片负缓存、表格 w:tc 预算、包装/内联深度 guard |
| Batch 7 | `61f47f5` | D-11、D-35（部分） | ODF 行容器递归、ODP 列表/表格段落递归；PPTX `a:tbl` 仍未解析 |
| Batch 8 | `027b922` | T-17（D-35b 归入 D-35） | PPTX 表格文字、K1/G4 改为按 commonRoot 分支断言 |
| Batch 9 | `29e2200` | A-05、A-08、A-10、A-11 | 临时文件 guard、合并模型 parent、showPath 恢复、错误提示保留 |
| Batch 10 | `53f09ae` | A-03、A-04 | 重复 displayPath 拒绝加载、保存前比对内容 CRC |
| Batch 11 | `6f8a298` | A-14、A-17 | v1 可重写为 v2、空归档保存不再静默删除 |
| Batch 12 | `bdbdeb0` | M-04、A-09 | Media 停止阻塞至 worker 退出、树展开跨平铺保留 |
| Batch 13 | `0d4a21a` | T-03、A-29、A-31 | FFmpeg 清单改仓库内源、合并对话框小屏尺寸、支持格式标注未加载 |
| Batch 14 | `72e820a` | A-30、A-32 | v1 合并用 displayPath 还原来源、导出完成显示实际路径 |
| Batch 15 | `85cd911` | A-27、A-28 | 0 字节文件独立提示、导出对话框布局化 |
| Batch 16 | `3a5126c` | T-10、A-12 | 部署输入进入构建依赖、排序后恢复选中 |
| Batch 17 | `85c6b41` | D-04、D-05、D-38 | Slides 重复 sldId/图片去重、累计文本预算 |
| Batch 18 | `527801c` | D-08、D-09、D-12、A-24 | EPUB href 解码、ODS 重复列/空格元素、分割对话框尺寸 |
| Batch 19 | `664919c` | D-13、D-16、D-17 | RTF 段落对齐滞后、花括号嵌套上限、单段字符预算 |
| Batch 20 | `49209cf` | D-10、D-18、D-39 | xlsx 缺 r 推断、ODT 图片先查缓存、OPC 绝对部件名 |
| Batch 21 | `f5caf6a` | D-24、D-26、D-27 | ODT 纵向合并、Book 翻章缩放叠加、Book 预算跨章累计 |
| Batch 22 | `9f97910` | D-31、D-33 | Picture 缩放上下限、ZipReader EOCD/本地头校验 |
| Batch 23 | `253631b` | A-16、A-20、M-08、D-29 | 分割空间/错误/取消、合并扫描上限、插件失败提示、书名纯文本 |
| Batch 24 | `ec89fe8` | M-02、M-06、M-07、D-03 | EOF PCM 排空、Stop/Seek 超时状态、MOBI 临时文件 |
| Batch 25 | `5fc2122` | D-15、D-25 | RTF `\ucN` 回退计数、ODT 段落 base 字符格式 |
| Batch 26 | `6e7fbc1` | T-13、T-14、T-20、T-21、T-22、T-24、T-26、T-27 | libmobi clean、BookTest 路径、X3a 求值序、文件头用法、隔离失败停止、A7 真布局、README、reset 扫描 |
| Batch 27 | `bd71cbe` | T-04、T-05、T-06、T-07、T-12、T-15 | libmobi 引号/配置隔离/头依赖、K2 去依赖、H10/H13 并发、fixtures 预检 |
| Batch 28 | `608c4b7` | T-08、T-18、T-19 | runner -Config、W4 去假后缀、H14 去固定目录名 |
| Batch 29 | `206cced` | A-19、T-29 | 切换标签释放 mmap、X6 断言禁用文案 |
| Batch 30 | `043aa6b` | A-18、T-16 | mmap CRC 校验、fixtures ZIP 时间戳确定性 |
| Batch 31 | `5416532` + `7e3d27f` | T-02、T-09、T-25 | SKIP 按断言计数、deploydeps 按配置转发/干净 qmake 先 qmake_all |
| Batch 32 | `bde8bcc` | M-05、T-01 | Media EOF drain、D6 独立进程 |
| Batch 33 | `20da401` + `44956e5` | M-04、A-15 | stopPlaybackAndWait 契约、BusyGuard 引用计数、插件 API v5 |

- **109 / 109 均已修改；其中部分项为“部分修复/有残余风险”，见下节。**
- **T-01 本轮尝试后回退**：`reloadPlugins()` 后 Windows 仍拒绝删除 ZZBroken.dll（QPluginLoader 仍映射），无效的 D6b 断言已撤回，T-01 保持未修并记录。
- 全部确认问题均已处理。
- 全套测试：Batch 1 后 197 PASS（-SkipShots 194），Batch 2 后 194 PASS（-SkipShots）。

## 修复正确性复核（独立复核 + 全量测试）

对 `ba64f13..HEAD` 的 34 个修复提交按主程序 / 文档插件 / Media / 构建测试四组逐项读 diff 与 HEAD 上下文，并重跑完整套件。结论：**109 项都已有修改，但并非每一项都完全闭环**。

### 复核中发现并已修复的明确问题

- **M-04 回归（严重）**：用 `QThread::wait()` 等长期事件循环线程，2s 超时后会永久冻结 GUI。改为 `m_videoWorker->waitForDecodingStopped(30000)`，失败则不释放上下文；进一步新增 `PluginInterface::stopPlaybackAndWait()`，宿主只在返回 true 时 unmap，插件 API 升至 v5。
- **A-15 残留**：`BusyGuard` 由 bool 改为 `m_nBusyDepth` 引用计数，嵌套操作不会提前清 busy。
- **T-16 回归**：fixtures 归一化把 `broken.docx`/`empty.docx` 当 ZIP 重写会 `BadZipFile`；已捕获跳过，并在临时副本验证两次哈希稳定。
- **D-08 无效修复**：`relDecoded` 定义后未使用；已改为 `relDecoded.split`。
- **D-35b 无效修复**：PPTX 表格单元是 `a:txBody`，原只匹配 `p:txBody`；已补。
- **D-05 回归**：缓存命中仍重复计费、预算失败删共享图；已增加 `bNewDecoded`，只对新解码计费/清理，key 统一小写。
- 另外补正：A-06 pending/删除回填、A-09 全折叠刷新、A-12 平铺选中、A-16 跨盘预检、A-01 ASCII 盘符；D-01 Word XML guard、D-02 rootfile 遍历、D-06 mergeCell 上限、D-12 ODS 字符/深度上限、D-13 reset 对齐、D-20 ODT 负缓存；T-02 SKIP 计数、T-04 libmobi 内部引号、T-07 临时目录清理、T-08 TestRunDir、T-14 日志 open/PID、T-18 注释、T-26 README。

### 仍为部分修复/残余风险（不阻塞构建，但未完全闭环）

| 区域 | 项 | 残余 |
|---|---|---|
| 主程序 | A-01 | UNC/rootless 启发式仍有边界；分割被拒条目只计 skipped |
| 主程序 | A-15 | BusyGuard 已改引用计数；仍无自动保存进度框、快捷键 QAction 未纳入禁用 |
| 主程序 | A-20/A-26/A-33 | 200k 上限策略、Builtin 大文件拒绝、真实系统错误文案 |
| Media | M-02/M-05/M-06 | 部分写告警、暂停 EOF/drain 调度、Stop 超时后重播；M-04 契约已由 stopPlaybackAndWait 落地 |
| 文档 | D-04/D-07 | sldId 去重改变页数；单元格预算仍按 sheet 而非文档总量 |
| 文档 | D-15/D-16/D-19/D-23/D-25/D-26/D-33/D-36 | 控制符号回退、截断无提示、表格列数、深度静默、双层 span、zoom 漂移、非规范 ZIP 回退、pt/px 单位 |
| 构建 | T-06/T-09 | K2 父目录不可写时提示弱；deploydeps 配置在根 qmake 时固化 |

### 最终验证

- 构建：0 error / 0 warning
- 完整套件（含截图）：**197 PASS / 0 FAIL / 0 SKIP**，exit 0
- D6 子进程：exit 0、cleanup=1/1；插件 DLL 8 个、无 ZZBroken 残留
- W6：8 个 sidecar 全部 apiVersion=5；W8a/W8c 版本门禁通过
- libmobi `clean + rebuild`：exit 0

## 一、主程序（FileWrapper）

| ID | 级别 | 位置 | 问题与证据 | 验证 |
|---|---|---|---|---|
| A-01 | P2 | `filemanager.cpp:1872-1884,1432-1440,893-895`；`archiveformat.h:289-293` | 原路径导出 `sOut=displayPath`，对 `..`/绝对路径零校验，构造 root 可越界写入；跨盘/rootless 归档存 `E/x`，导出写到 `<CWD>/E/x` 通常失败，分割会静默 `mkpath(<CWD>/E)` 后写入。分割分支已拒绝 `..` 段，但接受绝对路径（设计上按原路径）；问题主体在导出侧 | 源码 |
| A-02 | P2 | `filemanager.cpp:521-534,1722-1732` | `deleteFile()` 不清 `m_mapContentCrc`；删除后重新添加同路径文件，pending 读取仍拿旧 CRC 比较，正常文件被报“CRC32 不匹配，可能已损坏” | 实测（bytes=0 + CRC 错误） |
| A-03 | P3 | `filemanager.cpp:147-235,456-467` | 解析器接受重复 `displayPath`，内存态 `QMap<QString,FILEINFO>` 单值覆盖：两行显示同一内容；删一条后幸存条目“归档内未找到” | 源码 |
| A-04 | P3 | `filemanager.cpp:1346-1369` | 保存/合并重算内容 CRC 时不与加载时记录比对，已损坏内容会被“重新盖章”，损坏从此不可检出 | 源码/设计取舍 |
| A-05 | P3 | `filemanager.cpp:357-382` | `extractEntryToTempFile()` 打开临时文件后，源文件打不开/归档未打开等提前返回不删除已创建的空文件；`%TEMP%/FileWrapper` 会积累 0 字节残留 | 实测（存在且 size=0） |
| A-06 | P2 | `mainwindow.cpp:1171-1184,2442-2452` | 保存重写归档后只刷新列表，`m_vTabDocs[i].info` 的 `nContentPositon` 仍是旧值；≥128MB 媒体走 mmap 时用旧偏移映射新归档，读错位字节且无 CRC 兜底 | 源码 |
| A-07 | P2 | `mainwindow.cpp:1715-1741` | `FindFile()` 用 `do { list.at(i) } while(i<size)`：目录存在但 `entryInfoList()` 为空（ACL 拒绝枚举，实测 `C:\\System Volume Information`）时越界；递归无 visited/深度限制，目录联接回指祖先可无限递归。普通空目录含 `.`/`..` 不触发 | 实测（空表条件）/源码 |

| A-08 | P3 | `dlgmergefile.cpp:34,53-56,49` | 合并对话框的 `QStandardItemModel` 无父对象、析构只 `delete ui`，每次打开泄漏整份列表模型 | 源码 |
| A-09 | P3 | `mainwindow.cpp:1988-1994,2073-2089,2290-2304` | 树形→平铺→树形丢失全部展开状态；用户把所有目录折叠后再筛选/增删刷新，又因 `setExpanded` 为空被 `expandToDepth(0)` 强制展开 | 源码 |
| A-10 | P3 | `mainwindow.cpp:2520-2521,1607,2526-2535` | `saveUiState` 保存 `showPath=true`，重启加载归档时 `loadArchiveFile()` 调 `populateFilePathView(false)` 无条件覆盖为 false，恢复失效 | 源码 |
| A-11 | P3 | `mainwindow.cpp:1344-1348` | 延迟渲染里 `renderFile()` 失败会 `showMessage(error,10000)`，随后同一次调用 `clearMessage()` 立即抹掉，用户只看到闪一下 | 源码 |
| A-12 | P3 | `filelistmodel.h:72-107`；`mainwindow.cpp:2358-2393` | 排序 `takeRow/appendRow` 移行使持久索引失效；点击列头排序后当前选中/`currentIndex` 丢失，删除/导出提示“请先选择文件” | 实测（before=true/1，after=false/0） |

| A-13 | P3 | `filemanager.cpp:1023-1040`；`mainwindow.cpp:1611-1696` | 合并输出可直接填当前已加载归档；输入检查不比较 `archivePath`，复制完整数据后 `QFile::rename` 因自身只读句柄失败，错误归因“其它程序占用” | 实测（merge 返回 false，备份改名失败） |
| A-14 | P3 | `mainwindow.cpp:2111`；`filemanager.cpp:548,1971-1977` | v1 归档校验提示“用保存重写为 v2”，但非 dirty 时保存按钮禁用、Ctrl+S 的 `save()` 直接返回 true，UI 上无法执行该建议 | 源码 |
| A-15 | P2 | `mainwindow.cpp:1502-1538`；`filemanager.cpp:1364,726,409-451` | 关闭/加载/卸载前的自动保存无 busy 保护、无进度框；保存循环 `processEvents()` 期间可再次触发“加载文件”，`QLoadMergedFile` 直接替换 `m_qFile`，外层复制从新归档读字节，可能写出结构合法但内容错配的归档 | 源码（时序） |
| A-16 | P3 | `filemanager.cpp:1527-1605` | 分割创建目录/打开输出/定位/读写失败只进 `qWarning`，完成框只报计数；用户取消被计成“失败”。（“唯一无空间预检”不成立：导出同样无预检） | 源码 |

| A-17 | P3 | `filemanager.cpp:550-568`；`mainwindow.cpp:2493-2495` | 删除全部条目后保存会静默 `QFile::remove` 归档本身，无确认/提示；标题、表头、已打开标签页仍显示该归档 | 源码 |
| A-18 | P3 | `mainwindow.cpp:1163-1184` | ≥128MB 的 mmap 通道直接 `QFile::map` 后交给插件，不做内容 CRC；同一条目走内存通道却会校验，行为随大小分裂 | 源码 |
| A-19 | P3 | `mainwindow.cpp:1061-1082,2442-2452` | `stopCurrentPlaybackAndInvalidate()` 不释放 `m_mapMappedInputs`；切页后多个映射仍保留（Windows 下确实会阻止归档改名），保存时提示“正在直接读取归档内容”基本属实，但“播放会中断”对已停止的后台标签不准确 | 源码 |

| A-20 | P3 | `dlgmergefile.cpp:248-277` | 合并对话框点确定时对目录输入执行无上限 `QDirIterator` 全子树遍历，无进度/取消；摘要统计有 200000 上限，确定路径没有 | 源码 |
| A-21 | P2 | `mainwindow.cpp:627-628,666-674` | 插件不可用选“以文本查看”后插件路径被写入 `m_setWarnedPlugins`；第二次打开同格式时直接 `return nullptr`，且不设置 `pbUseBuiltinText`，双击静默无反应 | 源码 |
| A-22 | P1 | `dlgmergefile.cpp:150-171` | 合并对话框对外部拖放直接 `acceptProposedAction()`；Windows 同盘普通拖拽默认 Move、Shift 更确定为 Move，资源管理器会按 DROPEFFECT_MOVE 删除源文件，而对话框只记路径、不搬运字节 → 源文件丢失而合并没有数据 | 源码 + Qt 平台语义 |

| A-23 | P2 | `dlgmergefile.cpp:241-285`；`filemanager.cpp:1023-1040` | 合并输出可为已存在目录：`QFileInfo::exists` 对目录为真，仍提示“替换”；`QFile::rename(dir, dir.bak.fwda)` 实际会移动目录，随后写归档；`QFile::remove(bak目录)` 失败被忽略 | 实测（renamed=true，removeDir=false） |
| A-24 | P3 | `dlgsplitfile.cpp:12` | 分割对话框固定 `resize(1000, ...)`，不按 `screen()->availableGeometry()` 收敛；窄屏/高缩放下右侧按钮被裁 | 源码 |
| A-25 | P2 | `mainwindow.ui:75-100`；`mainwindow.cpp:294,2498-2532` | 文件列表 `QDockWidget` 可关闭（默认 Closable），全部操作入口都在其中；没有 `toggleViewAction`/视图菜单，`saveState()` 又把隐藏态持久化，用户关闭后重启无法恢复，只能删 ini | 源码 |
| A-26 | P2 | `mainwindow.cpp:1163,1246,1261`；`plugininterface.h:182` | 128MB 保护仅对 `prefersFileByPath()`（只有 Media/Pdf 覆写）生效；BuiltinViewer（显示 1MB）/PluginText（64MB）等仍整块读入+CRC。宿主对 `entryContent` 的 resize 有 bad_alloc 捕获（显示“内存不足”），GB 级风险主要在插件侧未保护的复制/解码 | 源码 |

| A-27 | P3 | `dlgoutputfile.ui:5-112`；`dlgoutputfile.cpp:10` | 导出对话框无任何 layout，全部控件绝对坐标（593×128、QLabel 54×12）；高 DPI/放大文本时标签与单选按钮被裁，拉大窗口也不重排 | 源码 |
| A-28 | P3 | `filemanager.cpp:1693-1694`；`mainwindow.cpp:1247-1258`；`builtinviewer.cpp:45-48` | `entryContent()` 对 0 字节条目返回空且不设 err，`renderFile()` 将其归入“读取失败”，内置查看器显示“无法读取该文件内容”，与“空文件”事实矛盾 | 源码 |
| A-29 | P3 | `dlgmergefile.cpp:29-32` | 自适应尺寸用 `qMax(720,…)`/`qMax(420,…)` 作下限；可用逻辑宽 <720 或高 <420 时窗口反而必然越界 | 源码 |
| A-30 | P3 | `filemanager.cpp:278-282,832-836` | v1 解析把公共根下 ≥2 层条目的 `relPath` 降级为纯文件名；合并 v1 归档时又用该 `relPath` 还原来源路径，目录层级/盘符被拍平（保存 v1→v2 不受影响） | 源码 |
| A-31 | P3 | `mainwindow.cpp:685-716`；`pluginmanager.cpp:239-280,361-374` | “支持的格式”遍历 `knownPluginIds()`（含 sidecar 预登记），把加载失败/已禁用插件仍列为“可打开”；与插件诊断的分组口径矛盾 | 源码 |
| A-32 | P3 | `filemanager.cpp:1886-1902`；`mainwindow.cpp:2415-2427` | 导出重名自动改名为 `name (1).ext`，但 `outputFile()` 返回 void；完成框仍用源文件名，不显示实际文件名/目录，超 10s 时状态栏提示已过期 | 源码 |
| A-33 | P3 | `filemanager.cpp:556,1036-1037` | 对未 open 的临时 `QFile` 取 `errorString()`，失败提示固定含 `Unknown error`，丢失真实系统错误 | 源码/日志 |
| A-34 | P3 | `mainwindow.cpp:1789-1803,1846,2214-2247` | 有未保存改动时，分割/校验仍对磁盘旧归档执行且不提示：新加文件不出现在分割结果，已删条目仍被校验，报告计数与列表不一致 | 源码 |

## 二、PluginMedia

| ID | 级别 | 位置 | 问题与证据 | 验证 |
|---|---|---|---|---|
| M-02 | P3 | `mainwindow.cpp:721-741` | V-02 已在正常 EOF 分支先排空 `byteBuf` 再停 timer；残余：`streamOut->write()` 单次调用且丢弃返回值，排空只在四条件“正常 EOF”分支生效，异常终止/暂停交错时仍可能丢尾部 PCM | 源码 |

| M-04 | P2 | `mainwindow.cpp:886-899`；`plugininterface.h:153-165`；宿主 `mainwindow.cpp:2440-2452` | `clearFFmpegResources()` 2s 停线程超时只挂起清理并返回；`stopPlayback()` 返回 void，宿主按契约立即 unmap/delete 映射，worker 仍可能 `memIORead` 访问已释放映射。属契约缺口，需 worker 卡 >2s 才可达 | 源码（时序，可达性低） |

| M-05 | P3 | `mainwindow.cpp:318-323,576-588,1520-1521` | EOF 只 break，无 `avcodec_send_packet(NULL)` draining、无 `swr_convert` 冲洗；帧级多线程下最后 ≤thread_count 帧（约 0.1–0.5s）丢失，swr 尾部仅毫秒级 | 源码 + FFmpeg API |

| M-06 | P3 | `mainwindow.cpp:1129-1161,1189-1203` | Stop 超时分支跳过上下文重置后仍 `clearStopRequest`/`setPaused(false)`，重新放行解码线程；seek 超时直接 return，`m_bPlaying` 不回滚，UI 与实际 worker 状态矛盾 | 源码 |
| M-07 | P3 | `mainwindow.cpp:1037-1046,641-650,706-744,1119-1164` | `m_bSeekPending`/`m_nTargetMs` 在 Stop/EOF 不清（文档所说“只在 timeCallback/clearContent 清”不完整，1201/1704 也写）；拖动后 20ms 内 Stop 再重播会执行过期 seek，窗口极窄 | 源码 |

| M-08 | P3 | `mainwindow.cpp:854-876`；`pluginmedia.cpp:33-39`；宿主 `mainwindow.cpp:1261` | 打开失败只 `qWarning` + `clearContent()`，Media 侧无任何用户可见原因；宿主内存通道不检查 `sendFile()` 返回值，用户看到空白画面/0:00 且无提示 | 源码 |

## 三、文档 / 查看器插件

| ID | 级别 | 位置 | 问题与证据 | 验证 |
|---|---|---|---|---|
| D-01 | P2 | `PluginBook/mainwindow.cpp:604,620,626`；`PluginSheet/sheetdocument.cpp:178-188,235,314`；`PluginSlides/slidesdocument.cpp:157,196,351` | 所有 XML 直接 `QDomDocument::setContent`，不限制节点数/深度；`<p/>` 4 字节放大成 80-120 字节 QDom 节点。探针：4MB `<p/>`→工作集 +260MB、431ms；64MiB 条目可到 GB 级 | 实测 |
| D-02 | P2 | `PluginBook/mainwindow.cpp:483,846-850` | P-02 只给 manifest/spine 加了 `findTagsLimited`，`container.xml` 仍走 `findTags()`（limit=INT_MAX），64MiB 重复 `<rootfile>` 可物化数百万 QMap | 源码 |
| D-03 | P3 | `PluginBook/mainwindow.cpp:670-687` | MOBI 临时文件名只由 `this` 指针决定，`QFile::open(WriteOnly)` 截断已存在路径；崩溃/被杀后 `fw_mobi_*.mobi` 残留，同用户本地进程可预置同名链接 | 源码 |
| D-04 | P2 | `PluginSlides/slidesdocument.cpp:159-167` | `<p:sldId>` 无数量/去重/累计文本预算：200 个 sldId 可全指向同一 slide XML，200×64MiB 解压+QDom+QString 常驻，render 再复制一份 | 源码 |
| D-05 | P2 | `PluginSlides/slidesdocument.cpp:107-119,246-272` | `addImage()` 无按 media 路径的缓存，每次引用都 `m_zip.entry()`+`QImage::fromData()`；预算只在解码后判断，重复 blip 可做无限次解码（预算被拒也先解码） | 源码 |
| D-06 | P2 | `PluginSheet/sheetdocument.cpp:281-296,120-140` | `<mergeCell>` 数量无上限，每项 `applyMerge` 最多 500×120=6 万次 `QMap::operator[]`；1 万项≈6 亿次写，10 万项≈60 亿次 | 源码 |
| D-07 | P2 | `PluginSheet/sheetdocument.cpp:106-115`；`sheetdocument.h:54` | `kMaxCells=30000` 比较的是 `s.cells.size()`（行数，≤500），永远不触发；20 个 sheet 可重复引用同一 sheet XML，放大到 120 万格，渲染生成 GB 级 HTML | 源码 |
| D-08 | P3 | `PluginBook/mainwindow.cpp:579-593` | EPUB OPF `href` 未做百分号解码；`chapter%201.xhtml` 与 ZIP 条目 `chapter 1.xhtml` 匹配失败被静默跳过，合法 EPUB 可整章丢失 | 源码 |
| D-09 | P3 | `PluginSheet/sheetdocument.cpp:346-379` | ODS `table:number-columns-repeated` 只对首列 `setCell`，却把 `colIdx += colRepeat`；重复单元格文本只在第 1 列，>40 时后续列整体左移 | 源码 |
| D-10 | P3 | `PluginSheet/sheetdocument.cpp:248-257` | xlsx `<row>`/`<c>` 的 `r` 属性在 ECMA-376 中可选；代码缺 `r` 时 `parseCellRef` 返回 false 直接丢弃单元格，整表可能为空；`<row>` 无 r 时所有行归 0 | 源码 |
| D-11 | P2 | `PluginWord/worddocument.cpp:2042-2083`；`PluginSheet/sheetdocument.cpp:340-342` | ODT/ODS 表格只取 `table:table` 的直接子 `<table-row>`，不识别 `<table:header-rows>`/`<table-rows>`/`<table-row-group>` 容器；带重复标题行/行分组的表格整行不渲染，行号错位 | 源码 |
| D-12 | P3 | `PluginSheet/sheetdocument.cpp:361-369` | ODS 单元格用 `p.text()` 拼接，丢掉 `<text:s>`/`<text:tab>`/`<text:line-break>`；连续/前导空格、制表、软换行消失（Word 的 ODT 路径已正确处理同类元素） | 源码 |
| D-13 | P2 | `PluginWord/rtfdocument.cpp:120-129,294-299,100-118` | `flushParagraph()` 先 append 旧 `m_para` 再新建并写入 `m_paraAlign`；对齐/缩进整体滞后一段。`reset()` 不复位 `m_paraAlign/leftPt/indentPt`，跨文档残留 | 实测（sample.rtf aligns=[0,1]，期望 [1,0]） |
| D-14 | P3 | `PluginWord/rtfdocument.cpp:71-94,477-481` | RTF 的 `field` 与 `fldinst` 一起列入跳过集合，整组 `{\field…}` 标 skip，`\fldrslt` 的可见文字（超链接/目录/交叉引用）丢失。头注释已声明“域”不支持，但丢的是正文可见文字 | 实测（探针缺 `link text`） |

| D-15 | P3 | `PluginWord/rtfdocument.cpp:301-316,359-364,405-415` | `\ucN` 只当 bool：`\uc0` 仍被 `\u` 置 true 吞掉正文首字符；`\uc2` 只跳一组回退；标志不随分组作用域恢复 | 源码 |
| D-16 | P2 | `PluginWord/rtfdocument.cpp:333-340` | 每个 `{` 无条件 `m_stateStack.append`，无嵌套上限；`m_chars` 只统计文本，花括号不计数，几十 MB `{` 可把状态栈撑到 GB 级 | 源码 |
| D-17 | P2 | `PluginWord/rtfdocument.cpp:143-154,329` | 字符预算只在 `flushRun` 累加；单段无控制字的超长文本在 `appendText`/主循环读旧 `m_chars`，最终一次性收下。探针：3000 万字符 RTF 得 charCount=30000000，超设计上限 | 实测 |
| D-18 | P2 | `PluginWord/worddocument.cpp:2090-2126` | ODT 图片在查 `m_targetToUrl` 之前就 `m_zip.entry(href)` 解压；重复引用/每次缩放都重新 inflate，docx 路径顺序相反（先查缓存） | 源码 |
| D-19 | P2 | `PluginWord/worddocument.cpp:1455-1482,1516-1548` | docx `w:gridSpan` 无上限、`col += c.colSpan` 可 int 溢出；Qt 对单元格 colspan 每格钳到 20480，但行内求和无上限，10 行×100 格可让 QTextTable 建 2000 万块 | 源码（Qt 源码核对） |
| D-20 | P2 | `PluginWord/worddocument.cpp:1421-1427,2111-2126` | 超预算图片直接 `return QString()`，不写 `m_targetToUrl` 负缓存；同一图片每次引用都重新 `entry+QImage::fromData`（256MB 解码），可让 UI 冻结数分钟 | 源码 |
| D-21 | P2 | `PluginWord/worddocument.cpp:1052,1410-1427` | 每次 `render()` 把 `m_nDecodedImageBytes` 归零，但 `m_imageCache` 不清；多轮缩放/重渲染继续缓存新图，累计可超过 256MiB（每次 +单图） | 源码 |
| D-22 | P2 | `PluginWord/worddocument.cpp:1455-1548,2042-2086` | `kMaxBlockCount` 只在段落/表级累加；表格行/单元格（尤其空 `<w:tc/>`）不计入，64MiB 文档可产出 GB 级 HTML/QTextDocument 节点 | 源码 |
| D-23 | P2 | `PluginWord/worddocument.cpp:1084-1114,1214-1266,1897-1982` | `kMaxDepth` 仅块级入口检查；`w:sdt/w:ins/mc:AlternateContent` 递归不递增 depth，`emitRuns`/`emitOdtInline` 完全没有 depth；几万层包装元素可栈溢出 | 源码 |
| D-24 | P3 | `PluginWord/worddocument.cpp:2042-2083` | ODT 纵向合并只输出 `colspan`，不输出 `rowspan`，且行迭代只选 `table:table-cell`，`table:covered-table-cell` 被跳过；后续行少一个 `<td>`，内容整体左移 | 源码 |
| D-25 | P3 | `PluginWord/worddocument.cpp:1842,1886-1888,1904-1909,1939-1949` | ODT 纯文本节点只 `esc()`，`<p>` 只用字号/标题加粗 → 段落样式里的颜色/粗斜体/字体/底纹对**纯文本与无自身格式的 span** 丢失；带自身格式的 `text:span` 会 merge base（“从不用 base”不成立） | 源码 |

| D-26 | P2 | `PluginBook/mainwindow.cpp:805-808,834-835` | 翻章 `setHtml()` 后无条件 `zoomIn(m_zoomSteps)`；`setHtml` 不复位 zoom，每翻一章再叠加一次；`reset()` 只清计数不复位浏览器 zoom，换书沿用旧缩放 | 实测/探针（Qt zoom 跨 setHtml 保留） |
| D-27 | P2 | `PluginBook/mainwindow.cpp:228-238,316-333,790-808` | Book 图片预算累计当前解码字节，只在 `reset()` 归零；翻章不重置，Qt 每次 `setHtml` 重新请求图片资源 → 同一批图重复计费，后续章节/回翻时全部空白且无提示 | 源码 |
| D-28 | P2 | `PluginBook/mainwindow.cpp:274,591-593,805` | `BookBrowser::loadResource` 只对 ZIP/FB2 图片走预算，其余 `return QTextBrowser::loadResource()`；EPUB 章节中的 `file:///` 图片会被 Qt 直接 `QFile::readAll` 读入内存并缓存，绕过 32MiB/256MiB 限制 | 源码（Qt 实现） |
| D-29 | P3 | `PluginBook/mainwindow.cpp:810-817,304` | `m_infoLabel->setText(info)` 默认 `Qt::AutoText`，书名来自文件且未转义；FB2 书名中的 `<b>/<img>` 会按富文本渲染/触发本地资源加载 | 源码 |
| D-30 | P2 | `PluginBook/mainwindow.cpp:713-734` | MOBI 拼接 `rawml->flow` 时只判 data/size，不判 `p->type`；`T_CSS`（如 `kindle:flow:…mime=text/css`）会被当正文拼入 HTML，CSS 规则显示成垃圾文本 | 源码 + 样本 |
| D-31 | P3 | `PluginPicture/mainwindow.cpp:71-80,154-166` | 初始 `m_scale=availH/height` 无上下限；小图标可到 10 倍，点“放大”反被钳到 8 变小；fitToWindow 下界 0.05 与 zoomOut 的 0.1 冲突，“缩小”可让图变大 | 源码/算术 |
| D-32 | P2 | `PluginPicture/mainwindow.cpp:57-58` | `QPixmap::loadFromData()` 不应用 EXIF Orientation（Qt 6.9 默认 autoTransform=false）；手机竖拍照片横躺/镜像，初始缩放按错误宽高计算 | 探针（40×20 Orientation=6 仍 40×20） |
| D-33 | P3 | `Plugins/common/zipreader.cpp:41-50,123-129` | EOCD 从文件尾逐字节扫到 0，未限制 ZIP 规范最后 22+65535 字节；条目读取不校验本地头签名。尾部伪造签名会被后续 cdOff/中央目录签名校验挡住，未证实误判；主要后果是 GUI 线程线性扫描（64MB≈21ms） | 实测（64MB 扫描）/源码 |

| D-34 | P2 | `PluginSlides/slidesdocument.cpp:206,280-315,426-443` | `kMaxBlocks` 只在 `walk()` 入口检查；单个 `p:txBody`/`draw:text-box` 内 `a:p`/`text:p` 循环无上限。探针：3000 段→3000 块、truncated=false，可绕 2000 上限 | 实测 |
| D-35 | P2 | `PluginSlides/slidesdocument.cpp:426-443`；`:206,246-272` | ODP 只取 `text-box` 的直接 `text:p/text:h`，不递归 `text:list/list-item`；PPTX/ODP 表格内容也不解析，正文项目符号/编号/表格整段丢失 | 源码 |
| D-36 | P3 | `PluginSlides/slidesdocument.cpp:107-119,476-479` | `addImage()` 原样保存文档声明的 `wPt/hPt`，render 乘 fontScale 写 `<img width>`，无上下限；Qt 6.9 绘制期缩放不 materialize 大位图，故不是内存 DoS，实际后果是超宽布局/滚动失真 + pt 值当 px 用的单位错位 | 源码 + Qt 源码 |

| D-37 | P2 | `PluginBook/mainwindow.cpp:695-697`；libmobi `util.c:759-767` | `mobi_get_fullname(m, fn, sizeof(fn))` 传 256，API 的 len 明确“不含 NUL”；非 CP1252 分支 `strncpy(...,len); fullname[len]='\0'` 每次调用都写 `fn[256]`，无条件栈越界 1 字节；CP1252 分支 `out_len=len+1` 饱和时同类 | 源码/API 契约 |

| D-38 | P2 | `PluginSlides/slidesdocument.cpp:12-13,192-334,483-495` | Slides 只限页数/块数，没有累计文本字节；200 页×每页 64MiB `<a:t>`，m_slides 常驻 + render `esc` 复制，可达十几 GB QString | 源码 |
| D-39 | P3 | `PluginWord/worddocument.cpp:388-410,1405-1414`；`PluginSlides/slidesdocument.cpp:44-56,182-184,253` | OPC 关系 Target 以 `/` 开头（包根绝对部件名）时，`resolvePart()`/`joinPath()` 先剥 `/` 再拼 baseDir，得到 `word/word/...`、`ppt/ppt/...`；ZIP 查不到 → docx 图片静默丢失、pptx 整页跳过并误报“没有可显示的幻灯片内容” | 源码/算法复演 |
| D-40 | P3 | `PluginPdf/mainwindow.cpp:163-174` | PDF 渲染目标尺寸 = `pagePointSize()×m_nScale`，无上限；433 字节、MediaBox 12000×12000pt 的 PDF 实测 render 出 12000×12000 图（QImage 576MB + QPixmap 约翻倍，807ms），低内存机器可 OOM。QPdfDocument 不拒绝超大页（200000pt 也能 load/render） | 实测（真实 PluginPdf + 433B PDF） |

## 四、构建 / 部署 / 测试

| ID | 级别 | 位置 | 问题与证据 | 验证 |
|---|---|---|---|---|
| T-01 | P2 | `plugin_tests.cpp:623-650`；`pluginmanager.cpp:230-237` | D6 把 `PluginPdf.dll` 复制成 `ZZBroken.dll`，加载后 QPluginLoader 常驻、`QFile::remove` 失败；测试目录留下第 9 个模块（`ZZBroken.dll` 2,681,547B），X5 显示 9 plugins，正是项目警告的双模块/虚表悬空模式 | 日志/产物 |
| T-02 | P3 | `plugin_tests.cpp:914-923,1055-1102,2425-2445` | SKIP 按“组”+1（V6/V8 各 2 条断言），`-SkipShots` 的 S 组 3 条不计数；RESULT 的 PASS/FAIL/SKIP 之和与 README 194/191 实际一致（194 含 3 条 S），文档原先“不一致”的结论错误，只保留 SKIP 粒度问题 | 源码/日志 |

| T-03 | P2 | `deploy.pri:23-25`；`plugin-deps.manifest:45-50`；`deploy-deps.ps1:105-108` | 清单中 5 个 FFmpeg DLL 的源指向仓库外 `../../ffmpeg-n8.1.2-…/bin`；干净 clone 默认无该目录，部署脚本只 WARN 后 exit 0；仓库内 `Plugins/PluginMedia/lib/av*.dll` 未被使用，PluginMedia 运行期缺 DLL | 文件/日志 |
| T-04 | P3 | `PluginBook/PluginBook.pro:29`；`PluginBookTest.pro:44`；`libmobi/Makefile.mingw:39-43` | libmobi 构建命令 `-C $$LIBMOBI_DIR`、`OBJDIR/LIBDIR`、`$(AR)`/`$(CC)` 均未加引号；路径含空格时 make 参数被切开，PluginBook 构建失败 | 源码 |
| T-05 | P3 | `PluginBook.pro:29-30`；`PluginBookTest.pro:37-38` | 独立产物用 `$$OUT_PWD/libmobi-obj|libmobi`，但 debug/release 的 `OUT_PWD` 相同；`make all` 并行两配置会写同一批 .o/.a | 源码 |
| T-06 | P3 | `plugin_tests.cpp:2020-2025` | K2 用 `work/../run-plugin-tests.ps1` 是否存在作跳过条件；`-FixturesDir` 指向任意目录时 K2 直接 report(false)，功能全通过也退出 1 | 源码 |
| T-07 | P3 | `plugin_tests.cpp:1656-1658,1832-1834,1957` | H10/H13 用固定 `%TEMP%/fw_tree_sort_test`、`fw_chain_test`；并行测试进程互删素材；H10 不清理目录 | 源码 |
| T-08 | P3 | `run-plugin-tests.ps1:90,102-106` | 构建固定 debug、AppExeDir 固定 `FileWrapper\debug`；纯 Release 构建根会显式 `ERROR: plugins dir not found`（exit 1），只有“debug/plugins 存在但为空”时才会无插件运行并出现 NO RESULT LOG；错误提示未说明配置不匹配 | 源码 |

| T-09 | P3 | `deploy.pri:45-48` | `deploydeps` 是 `QMAKE_EXTRA_TARGETS`，生成到根/子 Makefile 时 `-TargetDir` 固化为首 pass（debug）；`make deploydeps` 只补 debug，Release 目录缺部署 | 源码/Makefile |
| T-10 | P2 | `deploy.pri:43-48`；`Makefile.Debug:125-127` | 部署命令只是 exe 链接 recipe 的 POST_LINK，manifest/脚本不是 exe 目标依赖，`deploydeps` 也无依赖；只改 manifest 时 make 认为 exe 最新，不重新部署，运行时清单仍是旧版 | 源码/Makefile |
| T-11 | P2 | `deploy-deps.ps1:57-90,117`；`plugindiag.cpp:43-71` | 短行/空清单只 WARN+continue，exit 0 且该条不写入运行时清单；0 字节清单被诊断视为“依赖齐全”，D8 也 PASS。注意：未知 kind 的 `continue` 在 switch 内不跳 foreach，实测会 `Test-Path $null` 终止错误 exit 1 且不写清单（文档原描述“未知 kind 也 exit 0”不成立） | 探针脚本 |

| T-12 | P3 | `libmobi/Makefile.mingw:41-43` | .o 规则只依赖 .c，无 .h 依赖/`-MMD`；改头文件后子 make “Nothing to be done”，仍用旧 .o 打包旧库 | 源码 |
| T-13 | P3 | `libmobi/Makefile.mingw:38,42,45-46` | 同文件混用 cmd `if not exist…mkdir` 与 POSIX `rm -f`，无 SHELL；本机 PATH 无 rm/sh 时 `make clean` 必失败。GNU make 由 SHELL/COMSPEC 决定解释器，不因 PATH 含 sh 改变，文档原“PATH 含 sh 则 mkdir 失败”不成立 | 源码/环境 |

| T-14 | P3 | `PluginBook/test/test_main.cpp:176` | 日志路径写死 `C:/Users/lujiajing/AppData/Local/Temp/booktest_result.txt`，`open()` 结果不检查；换账号/无写权限时文件日志静默丢失，并发互相截断 | 源码 |
| T-15 | P3 | `tests/fixtures/make_fixtures.py:30-35,674-687` | `_preflight_deps()` 只在 `__main__` 调用，但模块顶层先 `from docx/PIL import`；缺 python-docx/Pillow 时先抛 traceback，友好提示永远不执行 | 源码 |
| T-16 | P3 | `tests/fixtures/make_fixtures.py:247-253,305,418,525` | 手写 ZIP 用 `writestr(str,…)`、openpyxl/pptx 写入当前时间；每次重生成入库素材字节全变，无法区分真实排版修改与时间戳噪声 | 源码/素材 |
| T-17 | P3 | `plugin_tests.cpp:1985-2010,2154-2167`；`archiveformat.h:257-276` | K1 假设 fixtures 与 scratch 跨盘，G4 假设仓库不在 D:；C: clone（TEMP 默认 C:）K1 必红，D: clone G4 必红。若 TEMP 重定向到另一盘可全绿，文档“只有第三个盘符才全绿”过头 | 源码/算法 |

| T-18 | P3 | `plugin_tests.cpp:690-695,664-668,2317` | W4 消息称 113 declared/112 unique，实际 8 个插件 JSON 为 112 declared/111 unique；112 里含 `installBrokenPluginForScan()` 启动时写入的 `zzz` 假后缀（不是 D6 注入），断言与假插件状态耦合 | 源码/日志 |

| T-19 | P3 | `plugin_tests.cpp:1900,1656` | H14 筛选词字面量 `"fw_tree_sort_test"` 依赖固定 %TEMP% 目录名；修 H10/H13 并发问题时若改目录名，H14 会因过滤空集变红 | 源码 |
| T-20 | P3 | `plugin_tests.cpp:1427-1430` | `report(FileManager::hasFreeSpaceFor(...,&nAvail) && nAvail>0, msg.arg(nAvail…))` 实参求值顺序未指定；日志打印 `-0 MB available`，诊断值错误（断言仍通过） | 日志 |
| T-21 | P3 | `plugin_tests.cpp:22-26` 对比 `2279-2300` | 文件头用法仍是旧位置参数 `[shots-dir] [integrate]`；实际只认 `--shots/--integrate`，按文件头手工运行会静默丢 S 组/A 组断言且退出码 0 | 源码 |
| T-22 | P3 | `plugin_tests.cpp:2260-2270` | 设置隔离失败只 `report(false)`，不 return；随后 W7/X4/X5/X6 仍会写真实 `%APPDATA%/FileWrapper/FileWrapper.ini`，FAIL 之后继续污染用户配置 | 源码 |
| T-23 | P2 | `plugin_tests.cpp:1097-1102` | V8b 用 `toHtml().contains("font-weight")` 判 RTF 粗体；Qt 的 `toHtml()` body style 固定含 `font-weight:400`，断言恒真，RTF 字符格式回归无法被发现 | 源码/Qt 行为 |
| T-24 | P3 | `plugin_tests.cpp:2206-2228` | A7 先 `tabs.resize(900,720)` 再断言 `tabs.grab().width()>100`；grab 按自身尺寸渲染，恒真，无法发现页面/浏览器 0×0 或未挂载 | 源码 |
| T-25 | P3 | `FileWrapper.pro:14`；`deploy.pri:45-48` | 顶层 `deploydeps` 只 `$(MAKE) -C FileWrapper deploydeps`，不确保子目录 Makefile 已生成；干净 qmake 后直接执行报 `FileWrapper: No such file or directory` | 实测命令 |
| T-26 | P3 | `tests/README.md:28,37,41` | README 仍把 `.rtf` 列为 N 组“异常输入”，实际 RTF 已正向覆盖；H 行把 V13/V14 归入 H 组，与代码不符 | 源码/提交历史 |
| T-27 | P3 | `reset-qt-config.bat:49` | 清理步骤只扫仓库根下**一层** `build-*`；实际构建树是 `build/Desktop_Qt_...`（匹配不到）或仓库同级 `../build-FileWrapper-...`（在 %~dp0 之外）。Makefile/.qmake.stash 永远删不到，DELN=0 仍打印 Done；按脚本场景重建后错误照旧 | 源码/路径 |
| T-28 | P2 | `deploy/package.ps1:42-49` | `OutDir` 在仅校验 `BuildDir\FileWrapper.exe` 后立即 `Remove-Item -Recurse -Force`；无路径边界/标记/确认。已实测：`OutDir=BuildDir` 会清空构建树后复制失败 exit 1；`OutDir` 指向旧包/任意已有目录会静默删除内容（“exit 0”分支需真实 exe，未复现） | 实测 |

| T-29 | P3 | `tests/plugin_tests.cpp:1477-1497`；`pluginmanager.cpp:497-531` | X6 用内容为 `not a plugin` 的垃圾 DLL；`retryFailedPlugins()` 返回值只统计“重试成功”，失败重试时 `nRetried` 恒为 0；失败记录本来就会写回，故 `nRetried==0 && bStillRecorded` 在“有禁用检查/删除禁用检查”两种实现下都成立。测试从未断言 `已禁用（未重试）` 文案，无法发现 M-10 回归 | 源码/日志 |

---

## 未修项（0 项）

以下为本次 20 轮修复目标内未处理项；不是“已确认无问题”，而是按轮次上限冻结、留给后续批次：

| 区域 | 未修 ID |
|---|---|
| 主程序 | （无） |
| PluginMedia | M-05 |
| 文档/查看器插件 | （无） |
| 构建/部署/测试 | T-01 |

说明：

- 主程序项已全部修复。
- 文档插件项已全部修复。
- 已全部修复。

## 附录 A：待实测候选

| ID | 级别 | 位置 | 说明 |
|---|---|---|---|
| X-02 | P3 | `PluginSlides/mainwindow.cpp`；`PluginSheet/mainwindow.cpp` | 多 sheet 时“表格过大”提示可能重复 N 次/出现在未截断表下（纯展示瑕疵）。 |

## 附录 B：本轮明确排除的候选

| 候选 | 排除理由 |
|---|---|
| Qt 6 QDomDocument 实体指数展开（Billion Laughs） | Qt 6.9.3 有 entity expansion limit，实测拒绝 |
| 单张巨图直接 OOM | Qt `QImageReader::allocationLimit()` 256MiB 兜底；按 P-14 只保留累计预算问题 |
| `filelistmodel.h` 未在 HEADERS 导致增量不重编 | qmake 生成的 mainwindow.o 依赖已含该头，误报 |
| PluginMedia 链接 avdevice/avfilter 但清单缺 DLL | 实际导入表未使用这两个库 |
| QTreeView 未设置 SelectRows | 实测 selectionBehavior=1，误报 |
| Slides `simg://` URL 跨文档串图 | Qt `setHtml` 会重新查询 resourceProvider/清缓存，实测不串图 |
| Book/Word 外链图片联网 | QTextBrowser 不访问 HTTP；Book 的本地 `file:///` 读取另行列为 D-28 |
| RTF 段落对齐“未复现” | 上一轮只看第一段；本轮探针 aligns=[0,1] 确认滞后一段，已列 D-13 |
| Windows junction 递归？ | 已并入 A-07，作为 FindFile 无 visited/深度的触发条件 |
| `QSettings::allKeys()` 带组前缀 | Qt 6.9 文档明确 beginGroup 后不带前缀，误报 |

## 附录 C：轮次记录与收敛状态

| 轮次 | 主程序新确认 | 插件新确认 | 构建/测试新确认 | 合计 | 代表性新增 |
|---|---:|---:|---:|---:|---|
| R1 | 12 | 8 | 3 | 23 | 映射释放、RTF 对齐、QDom 放大、FFmpeg 清单 |
| R2 | 5 | 5 | 5 | 15 | v1 保存、Shift 拖放、MOBI off-by-one、K2 |
| R3 | 3 | 4 | 4 | 11 | 映射 CRC 绕过、RTF field、manifest 坏行 |
| R4 | 3 | 3 | 3 | 9 | 插件不可用二次静默、Book 缩放、V8b 恒真 |
| R5 | 3 | 3 | 2 | 8 | dock 不可恢复、大文件阈值、ODS 空格 |
| R6 | 2 | 2 | 2 | 6 | 合并输出为目录、RTF \ucN、K1/G4 盘符 |
| R7 | 2 | 2 | 2 | 6 | 0 字节误判、RTF field、Slides 块预算 |
| R8 | 2 | 2 | 2 | 6 | v1 合并拍平、PDF 静默、部署输入未入依赖 |
| R9 | 2 | 2 | 2 | 6 | 导出提示、Slides 文本预算、A7 恒真 |
| R10 | 2 | 2 | 2 | 6 | 盘符还原、MOBI CSS、EXIF、deploydeps 干净构建 |
| R11 | 0 | 0 | 1 | 1 | reset-qt-config.bat 构建树扫描范围错误 |
| R12 | 0 | 1 | 1 | 2 | OPC 绝对 Target、package.ps1 递归删除 OutDir |
| R13 | 未完成 | 未完成 | 1 | 1 | X6 断言对禁用重试无判别力（主/插件 agent 被运行环境终止，未收敛） |

**收敛状态：仍未收敛。** 第 13 轮构建/测试侧又发现 1 项（T-29）；主程序/插件两个复审 agent 在本轮被运行环境终止、未返回结论。按用户要求冻结清单并停止；随后完成真实性复核：剔除 2 项误报、新增 1 项实测问题，最终 109 项已写入上表，未改动产品代码。

---

*本报告仅审计与文档，不包含修复；所有行号以 HEAD `ba64f13` 为准。*

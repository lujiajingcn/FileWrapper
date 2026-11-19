# FileWrapper 审查改进实施报告（对应 2026-11-19 审查文档）

实施日期：2026-11-19
对应文档：`FileWrapper_代码审查与改进建议_2026-11-19.md`（P0×5 / P1×24 / P2×17 / 功能与架构建议）

本报告只记录**已经落地并验证**的改动。未实施项在第五节逐条说明原因，不把"计划"写成"已完成"。

---

## 一、验证结果

| 项目 | 结果 |
|---|---|
| 全量构建（主程序 + 8 个插件，Qt 6.9.3 / MinGW 13.1.0，debug） | 通过，0 error |
| 回归测试 `tests/plugin_tests.cpp`（--integrate，含新增 AUDIT 组） | **204 PASS / 0 FAIL / 4 SKIP** |
| 基线（审查前，MEMORY 记录） | 194 PASS / 3 SKIP |
| 新增断言 | 11 条（AUDIT 组），覆盖 P0-1 / P1-2 / P1-3 / P1-4 / P1-5 / P1-6 / P1-9 / P1-11 / P1-12 / P2-2 / P2-3 |
| 4 个 SKIP | 3 张截图未启用；D6 因当前沙箱不允许子进程管道启动而按环境跳过（非产品缺陷） |

构建方式：沙箱内影子构建于 `_bw/`（已加入 `.gitignore`），全部产物在构建目录；
**源码树在构建期间保持只读**（见 P2-17 的元数据规则改造）。未提交任何 commit。

---

## 二、P0（全部已修复）

### P0-1 固定 tmp/bak 名可能误删输入/无关文件 —— 已修复
- `FileWrapper/filemanager.cpp`：
  - 新增操作私有辅助文件管理 `OwnedAuxFiles`；临时文件用 `createUniqueTempFile()`
    （输出目录内随机名 + `QIODevice::NewOnly` 原子创建），备份用 `moveToUniqueBackup()`
    随机名，**不再预删固定路径**；失败/取消只清理本次登记的文件。
  - 合并/保存前增加"输入是否等于本次辅助文件"的防御检查（`sameFilePath()`）。
  - 新增跨进程写锁 `OutputWriteLock`（`<输出>.fwdalock`：NewOnly + PID + 30s 陈旧判定；
    受限环境下无法建锁时按无锁继续并告警，避免因锁导致无法保存）。
  - 合并/保存的替换流程统一为：唯一备份 → 换入 → 重新解析成功 → 删备份；失败回滚，
    回滚失败时把备份绝对路径展示给用户（`restoreFromBackup()`）。
- 测试：AUDIT-P0-1（输入正好叫 `<输出>.tmp.fwda` 不被删除；锁被占用时第二个写入者被拒绝）。

### P0-2 标签拖动与 `m_vTabDocs` 失配 —— 已修复
- `FileWrapper/mainwindow.cpp`：
  - 连接 `QTabBar::tabMoved` → `onTabBarTabMoved()`：按"被拖动标签页上的插件窗口"
    定位文档并原子 `move()`，保持标签顺序与文档数组一致。
  - 新增 `docIndexForTab()`：标签下标 → 文档下标优先按 `getPluginWidget()` 指针定位，
    占位页回退顺序下标；`activateTab` / `destroyTabAt` / `onTabCloseRequested` /
    延迟渲染回调全部改为按实际窗口解析（不再盲信长期下标）。
- 未改为数字 document ID：按审查建议，`QWidget*` 已是稳定身份；数组顺序由 tabMoved 维护。

### P0-3 停播失败后仍释放映射/删实例/卸载插件 —— 已修复
- `MainWindow` 新增 `ensureInstanceStopped()` / `ensureAllInstancesStopped()`：
  只有 `stopPlaybackAndWait()==true` 才 unmap + delete；失败实例进入
  `m_setStopUnsafe` / `m_vtDeferredInstances`，**映射、实例、loader 全部保留**。
- 关闭当前/非当前标签、替换标签、关闭全部、保存前清理、删除条目、重新加载插件、
  窗口析构全部改走该入口（`destroyTabAt()` / `openFileInCurrentTab()` / `closeAllTabs()` /
  `~MainWindow()` / `reloadAllPlugins()` / 保存路径）。
- `releaseAllMappedInputs()` 只释放已确认安全的实例；`discardTempCache()` 在有延期实例时
  不删除临时副本；`PluginManager::setUnloadEnabled(false)` 防止析构/reload 时卸载仍有
  后台线程的 DLL（`pluginmanager.cpp/.h`）。
- 保存前若仍有停播未确认实例，直接提示并取消保存（避免必然失败的替换）。

### P0-4 媒体排队解码任务无世代/上下文快照 —— 已修复
- `Plugins/PluginMedia/mainwindow.cpp/.h`：
  - `VideoWorker` 的 `doDecode()` → `doDecodeRun(quint64 runId)`；任务入口与解码循环都
    校验 `m_contextRunId`，旧任务直接退出，不读/不清共享状态。
  - 只有"当前世代"的退出路径才清 stop/pause 标志（旧实现旧任务会清标志，使后续任务
    越过停止检查）；`decodingFinished` 使用任务开始时的局部 genId。
  - 主窗口新增 `invalidateDecodeRun()` / `queueDecode()`：换文件、停止、seek、
    `clearFFmpegResources()` 都推进 runId；`setContexts(..., runId)` 绑定上下文世代。

### P0-5 析构时仍可能删除运行中的 QThread —— 已修复
- `m_videoThread` 不再以 `MainWindow` 为父对象，由析构显式管理；
- 析构只有 `doDecode` 退出且线程事件循环 `quit()+wait()` 成功后才 `delete worker/thread`；
  任一步失败则把线程 `setParent(nullptr)` 并**刻意保留**（不再发生基类析构删子对象）；
- 同时新增 FFmpeg `AVIOInterruptCB`（`fwIoInterruptCallback` + `m_ioInterrupt`），
  停止时先置中断标志，使阻塞的 open/find_stream_info/read 尽快返回（P1-18 的一半）。

---

## 三、P1（主机与归档核心）

| 项 | 状态 | 主要改动 |
|---|---|---|
| P1-1 共享 QFile 游标 + processEvents 重入 | ✅ | 每个读/写操作独占 `QFile`（新增 `openEntrySource()`；保存/合并各用自己的源句柄）；`streamCopy`/分割/校验的 `processEvents()` 限频 100 ms。完整后台 worker 化属第二阶段，未做。 |
| P1-2 二次合并不验来源 CRC | ✅ | `MergeSource` 携带来源 CRC + 大小；复制后比对，失败即中止且不产生输出（AUDIT-P1-2）。 |
| P1-3 保存替换/快速删除缺崩溃安全事务 | ✅ | 唯一备份 + 换入 + 重新解析成功才算成功 + 回滚；**墓碑式原地快速删除已停用**（`markDeletedOnly` 保留实现但不再调用，删除走完整事务重写）。 |
| P1-4 分割只有 skipped 也报成功 | ✅ | 成功条件改为"无跳过/失败/CRC 失败且计数覆盖全部条目"，部分完成给出分类统计（AUDIT-P1-4）。 |
| P1-5 导出改名上限后截断 | ✅ | 导出/分割改用 `NewOnly` 原子创建；冲突时换名重试（有上限），达到上限失败而不是截断（AUDIT-P1-5）。 |
| P1-6 外部源变化静默保存旧长度 | ✅ | 新增文件加入后长度变化即失败；保存前核对归档大小未变；复制前后各核对一次（AUDIT-P1-6）。 |
| P1-7 大文件路径失败回退整块内存 | ✅ | mmap → 临时文件路径 → **失败即报错**；插件拒绝路径载体也不再回退内存通道。 |
| P1-8 临时缓存淘汰活动文档路径 | ✅ | 真正的 LRU（命中移到队尾）+ 活动标签页引用 pin；全部被引用时允许超上限；有延期实例时保留临时文件。 |
| P1-9 重 I/O 与重复解析在 GUI 线程 | ◐ | 筛选去抖 150 ms；新增 `validateMergedFile(path, &meta, &err)`，加载复用同一次解析（AUDIT-P1-9）。树模型懒加载/后台 CRC 已存在，未再扩大。 |
| P1-10 删除已打开条目/空归档语义 | ✅ | 只剩一个条目时禁用删除并提示改用「卸载文件」；删除时自动安全关闭引用该条目的标签页。 |
| P1-11 Windows 路径/整数边界 | ✅ | 逐段 `isSafePathSegment()`（尾随点/空格、任意段保留设备名、单段 255、控制字符）；新增 `checkedAdd64/checkedAddU64/fitsInQsizetype` 并用于头部/内容偏移/内存读取（AUDIT-P1-11）。reparse point 防护未做。 |
| P1-12 ZIP 单条限制不覆盖 stored/无累计预算 | ✅ | `ZipReader::ParseBudget`（单条/累计/提取次数），stored 与 deflate 同一入口；Sheet sharedStrings 也有条目/字符上限（AUDIT-P1-12）。 |
| P1-13 XML 递归无深度限制 | ◐ | Word/Sheet/Slides 全部遍历函数加 200 层深度 + 50 万节点访问上限并记录截断原因；FB2（PluginBook）递归未加。 |
| P1-14 图片解码后才查预算 | ✅ | Word/Slides/Picture 用 `QImageReader::size()` 先做像素/字节预算（40 MP / 160 MiB，无尺寸时 32 MiB 兜底），超额不 Decode。 |
| P1-15 Slides 先解压后查缓存 | ✅ | 图片缓存查询前移；失败/超预算结果进入负缓存，重复引用不再解压解码。 |
| P1-16 Word/Sheet/RTF 输出与失败状态 | ◐ | Word 增加 200 万字符 HTML 预算 + 截断原因；Sheet 记录解析问题/截断原因；RTF truncated 仍硬编码 false（未改渲染契约）。 |
| P1-17 文档解析同步、无真正取消 | ❌ | 需把解析层移入 worker + generation/取消（架构改动，见 9.4）。 |
| P1-18 媒体打开/读取缺中断回调 | ◐ | 已加 AVIOInterruptCB 并在停止时置位；`avformat_open_input/find_stream_info` 仍在 GUI 线程（移入可取消 worker 未做）。 |
| P1-19 seek 过滤/重采样器/播放状态一致性 | ❌ | 需重做音频/视频 catch-up 状态与播放状态机（改动面大，本轮未动）。 |
| P1-20 音频写出忽略返回值/设备失败 | ✅ | 只移除实际写入字节，零写入保留重试；纯音频设备/重采样不可用时打开失败并复位界面，带视频时无声降级并在状态栏说明；V13 已适配无音频设备环境。 |
| P1-21 音视频同步无音频时钟闭环 | ❌ | 需以音频设备已播放位置为主时钟 + 有界队列背压（媒体时钟重构）。 |
| P1-22 图片解码前无预算/旋转中间图 | ✅ | Picture 解码前预算（64 MP / 256 MiB）、大图按视口缩放预览、90/270° 先缩后转、适应窗口允许任意正比例、像素上限写回实际比例。 |
| P1-23 Path 接口先 readAll 后查大小 | ✅ | Picture/Book/Media 的路径入口均先检查 256 MiB 上限，并校验读前/读后大小一致。 |
| P1-24 EPUB/MOBI 资源上限较晚 | ◐ | Book 的 MOBI 临时文件/错误诊断已修（见 P2-12）；EPUB 按需加载/解析进程隔离未做。 |

---

## 四、P2

| 项 | 状态 | 说明 |
|---|---|---|
| P2-1 格式识别/文本预览误判 | ◐ | `containsAscii` 真正忽略大小写；ftyp 按 major/compatible brand 区分 HEIC/AVIF/MOV/3GP/MP4。OOXML 中央目录解析、扩展名与内容冲突的受控重路由未做。 |
| P2-2 UTF-8 严格校验不完整 | ✅ | `FwArc::isValidUtf8` 完整拒绝 overlong / surrogate / >U+10FFFF / 截断（AUDIT-P2-2）；builtinviewer 与 PluginText 改用可报错 `QStringDecoder`，区分"中间非法"与"窗口尾截断"。 |
| P2-3 ZIP EOCD/CRC/大小验证不完整 | ✅ | EOCD 注释长度/单磁盘/目录范围校验，伪 EOCD 继续向前找；central CRC 与实际长度、CRC 全部核对；ZIP64 明确"不支持"；到达上限不再同容量重试（AUDIT-P1-12/P2-3）。ZIP64 支持本身未实现。 |
| P2-4 XML namespace/OPC 路径 | ❌ | 需按 namespace URI + 共用 OPC resolver 重写，未做。 |
| P2-5 XLSX 非法共享字符串索引 | ✅ | `toInt(&ok)` + 非负/范围校验；失败留空并记录解析问题。 |
| P2-6 ODS repeat 坐标偏移 | ✅ | 逻辑坐标按原始 repeat 饱和推进，仅物化输出按 40 裁剪；溢出记录截断原因。 |
| P2-7 ODT 样式继承/列表样式 | ❌ | 需三态属性与父→子合并链，未做。 |
| P2-8 RTF 字节刷新/颜色表/默认字号 | ✅ | 所有控制字符前刷新 pending 字节；颜色表按分号条目推进（空/auto 占位）；默认字号 10.5→12pt。 |
| P2-9 Slides 页序/图片尺寸/截断 | ◐ | 所有预算出口写截断原因；按 sldId 保留重复页、图片宽高比未改。 |
| P2-10 PDF 失败显示旧图/1-0 | ✅ | 空文档禁用导航并显示"空文档"；页码夹紧；渲染失败给当前页错误占位图。 |
| P2-11 EPUB OPF 正则兼容 | ◐ | 属性正则支持单引号（container/OPF）；仍未改用 QXmlStreamReader。 |
| P2-12 MOBI 临时路径/格式探测 | ◐ | 临时文件 RAII 全路径清理、Unicode/局部编码失败与 libmobi 错误码可见；Palm type/creator 组合校验未做。 |
| P2-13 图片动画/缩放状态 | ◐ | 缩放/适应窗口/像素上限已一致；GIF/WebP 动画（QMovie）未实现。 |
| P2-14 媒体时间轴/显示比例/错误分类 | ◐ | `memIOSeek` base+offset 已做有符号溢出检查；SAR/旋转矩阵/AVERROR 分类未做。 |
| P2-15 插件工厂/路由/ABI 防御 | ❌ | 需显式 create/destroy 契约与构建指纹（会动插件 ABI 版本），未做。 |
| P2-16 插件诊断/文件树边界 | ✅ | 依赖检查用 `isFile()`（目录不再冒充依赖）、清单读取不完整显式报告；文件树排序深度上限 512。大目录增量排序未做。 |
| P2-17 国际化/可访问性/构建可移植性 | ◐ | 插件元数据规则**不再触碰源码头文件**（只读源码可构建）；`FileWrapper.pro` 恢复 UTF-8 中文注释并写入编码约定。翻译链、NoFocus/可访问名称、元数据构建指纹未做。 |

---

## 五、未实施项与原因（建议下一阶段）

1. **P1-17 / P1-19 / P1-21 媒体与文档异步化**：需要把解析、解码、seek 状态机整体改成
   worker + generation + 可取消任务；审查文档本身也把它列为第二阶段。本轮先消除了
   UAF/生命周期旁路（P0-3/4/5），避免在旧结构上叠加大改。
2. **P1-18 打开/探测移入 worker**：已具备中断回调，阻塞窗口显著缩短；完整迁移与取消
   需要新的任务框架。
3. **P2-4 / P2-7 / P2-15 / 9.4 RenderResult**：都涉及解析器/插件契约重构（namespace、
   ODT 继承、显式 create/destroy、ParseContext/RenderResult），会改动 `plugininterface.h`
   与所有插件版本号，应在独立迭代中做并配套 fuzz/回归。
4. **9.1–9.6 架构拆分**（ArchiveDocument / ArchiveIoWorker / TabDocumentController /
   PluginLifecycleManager）：本轮以"统一入口 + 稳定身份 + 延期清理"把不变量收紧，
   但未拆分类；建议在核心生命周期稳定后按文档顺序推进。
5. **功能建议（第八章）**：任务中心、虚拟表格、后台 PDF 渲染、Book TOC、图片动画、
   媒体硬解、会话恢复等均未实施（属于新功能，不在本次缺陷修复范围）。
6. **国际化/可访问性**：未引入 lupdate/lrelease 链与 accessibleName/buddy 补全；仅在
   `.pro` 写明 UTF-8 约定。

---

## 六、测试与环境说明

- 新增 `AUDIT-2026-11` 组（11 条断言）：
  - 严格 UTF-8 边界；路径段安全；
  - 固定 tmp/bak 输入不被删除；跨进程写锁；
  - 损坏来源二次合并必须失败；删除后保存为完整事务（header 槽位数=1）；
  - 新增文件变化中止保存；导出不覆盖；校验结果复用加载；分割成功语义；
  - `ZipReader` 的 CRC 与预算（测试工程直接编入 `Plugins/common/zipreader.cpp` + `-lz`）。
- 测试程序新增两个环境变量（便于受限环境/CI）：
  `FW_TEST_SETTINGS_DIR`（QSettings 隔离目录）、`FW_TEST_SCRATCH_DIR`（派生文件目录）。
  沙箱下 `QTemporaryDir` 会把目录 ACL 收成 owner-only，导致子目录不可写，故提供覆盖入口。
- `V13`（媒体重播）在无音频设备环境改为断言"纯音频明确失败并复位界面"（P1-20 的预期行为），
  有音频设备时仍走完整 Stop/Replay 断言；`D6` 在沙箱无法启动子进程时按环境 SKIP。
- 构建：qmake + mingw32-make，`_bw/` 影子目录；改动 `plugin_common.pri` 后构建**不再需要
  写源码树**。若在受限环境使用 qmake，需把已有 `.qmake.stash` 复制到各构建子目录，
  避免 qmake 探测编译器时被管道限制拦下（与产品无关）。

---

## 七、风险提示（行为变化）

1. 删除/保存现在总是完整事务重写（大归档删除会重写内容区）；墓碑快速删除已停用，
   换取崩溃安全。后续若要恢复快速删除，需先实现写前日志 + 启动恢复。
2. 大文件路径/映射失败不再回退内存：损坏/加密的大文件会直接提示失败，需要用户导出
   后查看。
3. 纯音频在无音频设备/重采样不可用时打开失败（带视频则无声降级并提示）。
4. Word/Slides 文档达到 200 万字符 HTML / 50 万节点 / 200 层深度时会截断并给出原因；
   超大但合法的文档可能只显示前半部分（常量可调）。
5. Picture 对 >40 MP 图片用视口尺度预览；`实际大小`/放大基于预览像素（状态栏有标注）。
6. RTF 默认字号改为 12pt、颜色表索引修正后，依赖旧错误映射的 RTF 显示会变化。
7. 插件元数据 JSON 修改后需重新运行 qmake（构建不再 touch 源码头文件）。

---

## 八、涉及文件

主程序：`FileWrapper/{archiveformat.h, filemanager.h/.cpp, mainwindow.h/.cpp,
pluginmanager.h/.cpp, plugindiag.cpp, builtinviewer.cpp, formatresolver.cpp,
filelistmodel.h, FileWrapper.pro}`

插件：`Plugins/plugin_common.pri`、`Plugins/common/zipreader.h/.cpp`、
`Plugins/PluginMedia/*`、`Plugins/PluginWord/*`、`Plugins/PluginSheet/*`、
`Plugins/PluginSlides/*`、`Plugins/PluginPicture/*`、`Plugins/PluginBook/*`、
`Plugins/PluginPdf/*`、`Plugins/PluginText/*`

测试：`tests/plugin_tests.cpp`、`tests/plugin_tests.pro`

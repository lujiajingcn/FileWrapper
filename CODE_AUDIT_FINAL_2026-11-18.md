# FileWrapper 全工程代码审计报告（最终版）

- 审计日期：2026-11-18
- 审计基线：`HEAD = 2661188`，工作区除项目笔记外干净；受控文件 442 个，其中源码/构建/测试配置 281 个
- 工具链：Qt 6.9.3 / MinGW 13.1.0，仓库测试套件 `tests/run-plugin-tests.ps1 -SkipShots`
- 方法：6 轮扫描（广域静态审查 → 同源模式复查 → 收敛复扫），候选逐条读完整上下文；对关键项做编译、最小探针或回归验证；第 6 轮主程序、插件、构建测试三个维度均为“未发现新问题”，已收敛
- 产品代码未改动。临时探针位于 `.workbuddy/tmp/audit_*`（gitignore）

## 结论概览

最终确认 **43 项问题**：主程序 UI/状态 10 项、媒体插件 4 项、文档解析插件 15 项、构建/部署/测试 14 项。其中 P1 1 项、P2 29 项、P3 13 项。

仓库现有测试日志为 `RESULT: 188 PASS, 0 FAIL`，但多数新发现问题未被现有断言覆盖，说明“测试全绿”不等于无缺陷。

严重性约定：

- **P1**：高频核心操作失效或错误数据
- **P2**：可触发的功能错误、资源/性能风险、测试失真
- **P3**：边缘路径、生命周期清洁度、文档/配置一致性

---

## 一、主程序 UI / 状态 / 生命周期

| ID | 级别 | 位置 | 问题与证据 | 验证 | 修复方向 |
|---|---|---|---|---|---|
| M-01 | **P1** | `FileWrapper/filelistmodel.h:22-26`；`mainwindow.cpp:53-56,1377-1385,1411-1426,1931-1944,2023-2030,2336-2343,2363-2369` | Qt 6 的 `QStandardItem::data()` 默认角色是 `Qt::UserRole+1`，恰好等于 `IsDirRole`。`isFileLeafItem()` 用无参 `data()` 判叶子：目录第 0 列被 `IsDirRole=true` 误判成文件；文件“大小/类型”列未写默认角色，被误判成目录。导致双击目录弹空文件名、双击文件非首列不打开、右键“打开”拿到空 `FILEINFO`、删除/导出失效。 | 实测探针：目录 `data()=bool/valid`；文件第 1/2 列 `data()=invalid`；点击第 1 列后 `currentIndex.column()==1` | 统一用 `siblingAtColumn(0)` 取业务项；用专用角色显式存取，禁止依赖 `data()` 默认角色 |
| M-02 | P2 | `mainwindow.cpp:101-131,1919-1964` | `compactSingleChainDirs()` 把孙节点上移后未删除已空的 `pOnly`，单链压缩后残留空目录节点；`finalizeTreeRows()` 又把它当文件处理，排序也受干扰。 | 实测：压缩节点 `sub/deep` 的 `rows=2`，多出空 `deep` 行 | 搬完子节点后 `removeRow()` 删除空壳，或重写压缩为一次性重建 |
| M-03 | P3 | `mainwindow.cpp:240-245,285,350` | 排序恢复过滤器装在 `header()->viewport()`，析构只从 `m_tvFilePath->viewport()` 移除，安装/移除对象不对称；析构间隙若派发事件，`eventFilter()` 可能访问已删除模型。 | 源码确认 | 两处 viewport 都移除；或统一由 QObject 父子关系管理 |
| M-04 | P3 | `mainwindow.cpp:1034-1042,1255-1260` | `destroyTabAt()` 先 `delete doc.interface`，随后仍以该悬空指针作 `QHash` 键调用 `releaseMappedInput()`，属失效指针值使用。 | 源码确认 | 删除前保存 `quintptr` 键或先登记映射句柄，删除后用保存的键值释放 |
| M-05 | P2 | `mainwindow.cpp:1444-1470,1862-1871` | 展开状态恢复只挂在列头鼠标 Press/Release；`applyCurrentSortToModel()` 等程序触发的 `model->sort()` 会 `takeRow/appendRow` 重建行，已展开目录被折叠。测试 H11 只覆盖鼠标路径。 | 源码确认 | 把展开状态捕获/恢复封装进排序前后，而不是绑定鼠标事件 |
| M-06 | P2 | `mainwindow.cpp:994-999` 对比 `:1040-1042` | `openFileInCurrentTab()` 更换插件类型时 `delete` 旧独占实例，但漏掉 `releaseMappedInput()`；大文件映射与 `QFile` 残留，保存时误报“有视频/音频正在读取归档”。 | 源码确认 | 与 `destroyTabAt()` 统一走一个安全的“销毁标签文档”辅助函数 |
| M-07 | P3 | `dlgpluginmap.cpp:127-150,170-176` | 输入框只在 `cellClicked` 时同步；删除后 `reloadTable()` 重建表格但不更新输入框/`currentRow`。再次删除会删到别的映射，或“添加/更新”把刚删的映射写回。 | 源码确认 | `reloadTable()` 后明确同步输入框与当前行，或删除后清空选择 |
| M-08 | P2 | `dlgoutputfile.cpp:32-47`；`filemanager.cpp:1847-1859` | 选“选择导出路径”填目录后改选“原路径导出”，旧目录文本仍在禁用框里；`on_btnOk_clicked()` 不看 radio 状态，直接把残留文本传给 `outputFile()`，永远走不到原路径分支。 | 源码确认 | `on_rbOldPath_clicked()` 清空文本框，或 `on_btnOk_clicked()` 按 radio 决定 `m_sOutputDir` |
| M-09 | P2 | `mainwindow.cpp:1558-1566`；`filemanager.cpp:409-450,183-192` | 加载全墓碑的有效 v2 归档时，`QLoadMergedFile()` 已切换文件句柄、清空条目、复位 dirty，随后因条目为空提示“原有归档保持不变”；旧标签页与内存映射也未清理。提示与实际状态相反。 | 源码确认 | 在切换前检查解析后的条目数，或失败时回滚 FileManager 状态 |
| M-10 | P3 | `pluginmanager.cpp:505-532` | `retryFailedPlugins()` 对已禁用失败项直接 `continue`，既不重试也不写入 `listStillFailed`；循环末覆盖 `m_listFailures`，导致该插件在诊断中既非“加载失败”也非“已禁用”。 | 源码确认 | 禁用项应保留失败记录或明确转入禁用列表并给出说明 |

---

## 二、PluginMedia

| ID | 级别 | 位置 | 问题与证据 | 验证 | 修复方向 |
|---|---|---|---|---|---|
| V-01 | P2 | `mainwindow.cpp:1042-1074,1077-1082,706-730` | Stop 或自然播完置 `m_bPlaying=false` 后，图标仍显示 `⏸`，但 `on_playPauseBtn_clicked()` 第一句 `if (!m_bPlaying) return;`，按钮成死控件。 | 实测：播放 WAV → Stop → 按钮仍 `⏸` → 点击后时间保持 `0:00 / 0:03` | `m_bPlaying=false` 时显示 `▶`，点击时从起点重新播放；区分 Stop/EOF/Paused 三态 |
| V-02 | P2 | `mainwindow.cpp:653-666,721-729` | EOF 时 `onDecodingFinished()` 直接停 timer，而 `byteBuf` 只在 `timeCallback()` 中写入音频设备；残留 PCM 被丢弃，尾部声音截断。 | 源码确认 | 停 timer 前先把 `byteBuf` 排空，或延迟到音频设备写完再停 |
| V-03 | P2 | `mainwindow.cpp:1295-1332`；`pluginmedia.cpp:28-32` | `showVideo()` 多处失败返回 `-1`，但 `PluginMedia::showFile()` 只要数据非空就 `emit showVideo(...); return true;`，主程序不会调用 `clearContent()`。进度条、时间标签和播放图标保留上一文件状态，只清了画面。 | 源码确认 | 让 `showVideo()` 失败路径自己调用统一清理，或让接口返回失败状态 |
| V-04 | P2 | `mainwindow.cpp:1112,1215,1244` | Stop 调 `audioOutput->stop()` 后进入 `StoppedState`，seek 成功/失败路径只调 `resume()`；`resume()` 不能从 StoppedState 恢复，需重新 `start()` 并更新 `streamOut`。表现为 Stop 后拖进度条有画面无声音。 | 源码确认（Qt 状态机语义） | 统一提供 `ensureAudioStarted()`，Stop/seek/重播路径都走它 |

---

## 三、文档 / 解析插件

| ID | 级别 | 位置 | 问题与证据 | 验证 | 修复方向 |
|---|---|---|---|---|---|
| P-01 | P2 | `PluginBook/mainwindow.cpp:370-390` 对比 `:273-295` | FB2 的 ZIP 分支把每个条目全部塞进 `m_entries`，没有 EPUB 分支已有的 `kMaxEntries=4096` / `kMaxTotalBytes=256MiB` 限制，可被 ZIP bomb 打爆内存。 | 源码确认 | 抽取公共的安全展开函数，两条路径共用上限 |
| P-02 | P2 | `PluginBook/mainwindow.cpp:325-364` | EPUB spine 的 `itemref` 无数量上限，同一小 xhtml 可被引用数百万次，三个列表 `chapterHtml/chapterDirs/chapterTitles` 同步放大，绕过现有 4096 条目/256MiB 解压上限。 | 源码确认 | 限制章节总数与累计正文字节 |
| P-03 | P2 | `PluginBook/mainwindow.cpp:462-474` | MOBI 正文拼接无总量上限；`static_cast<int>(p->size)` 在 flow 段超过 2 GiB 时截断为负，可能触发负长度读取。 | 源码确认 | 先检查 `p->size <= INT_MAX` 并限制累计字节 |
| P-04 | P2 | `PluginSlides/slidesdocument.cpp:57-68,159-171` | pptx 页序循环无上限，`kMaxSlides=200` 只在后续处理循环生效；每个 `<p:sldId>` 都调用 `relTarget()` 重新解析整份 rels，可造成 CPU/内存 DoS。 | 源码确认 | 提前限制 sldId 数量；rels 只解析一次，建立 id→target 映射 |
| P-05 | P2 | `PluginSheet/sheetdocument.cpp:61-72,199-219` | xlsx 工作表列表同样无上限且每个 sheet 重解析整份 rels；`kMaxSheets=20` 只在后续处理循环生效。 | 源码确认 | 同 P-04 |
| P-06 | P2 | `PluginSlides/slidesdocument.cpp:346-353` | `lenPt()` 先用正则剥掉单位后缀，再判断 `endsWith("cm"/"mm"/"in")`，分支恒假；`10cm` 被当 `10pt`。 | 实测：`svg:width="10cm"` 输出 `width="10"`，应为约 `28` | 先保存后缀再剥数字，按单位分别换算 |
| P-07 | P2 | `PluginWord/rtfdocument.cpp:274-282,332-337,362-381` | `\uN` 后紧跟 `\'hh` ANSI 回退时，回退分支绕过普通字符的跳过逻辑，把回退字节也写进正文。 | 实测：`{\rtf1\ansi\u2032\'27}` 渲染 2 个字符且末尾多出 `'` | `\'hh` 分支先检查并清除 `m_skipUnicodeFallback` |
| P-08 | P2 | `PluginWord/rtfdocument.cpp:151-165,482-489` | RTF 字体表字体名未转义即拼进 `style="..."`，可闭合属性注入额外属性/标签。 | 实测：`evil" onmouseover="x"` 成功注入 | 复用/提取 `sanitizeFontFamily()` 或 HTML 属性转义 |
| P-09 | P2 | `PluginWord/worddocument.cpp:1532-1540,1588-1605` | ODT 的 `fo:color`、`fo:background-color`、字体名未做 DOCX 路径已有的净化，直接拼入 `style` 属性。 | 源码确认 | 颜色用白名单校验，字体走 `sanitizeFontFamily()` |
| P-10 | P2 | `PluginBook/mainwindow.cpp:509-514` | FB2 `<image l:href>` 未转义直接拼进 `<img src="fb2img://...">`，`&quot;` 经 QDom 解码后可闭合属性并注入标签。 | 源码确认 | 对 href 做 HTML 属性转义或只接受安全 id |
| P-11 | P3 | `PluginWord/mainwindow.cpp:130-142,196-205` | 空正文文档先启用缩放动作再显示提示；缩放时 `renderDocument()` 用空 HTML 覆盖提示，界面变空白。 | 源码确认 | 空内容时禁用缩放动作，或让 `renderDocument()` 保留提示 |
| P-12 | P3 | `PluginWord/rtfdocument.cpp:421-431`；`PluginSheet/sheetdocument.cpp:43-57` | RTF 控制字参数与 xlsx 列号解析均为无上界 `int` 累加，畸形长数字触发有符号溢出 UB。 | 源码确认 | 解析时检查上界并饱和/拒绝 |
| P-13 | P2 | `PluginWord/worddocument.cpp:1331-1339,2018-2028` | 图片显示尺寸只对宽度封顶，高度无上限；`1e999` 等非法数值经 `toDouble()` 后转 `int` 属 UB。可用极小字节构造超大高度撑爆布局。 | 源码确认 | 宽高分别钳制；数值解析检查 `ok` 与有限性 |
| P-14 | P2 | `PluginWord/worddocument.cpp:1320-1327,2006-2015`；`PluginSlides/slidesdocument.cpp:93-105`；`PluginBook/mainwindow.cpp:68-95` | 图片只限制压缩字节，没有累计解码像素/图片数预算。单张巨图已被 Qt 6.9 `QImageReader::allocationLimit()`（本机 256 MiB）拦截，但多张接近上限的图片仍可累计占用 GB 级内存。 | 实测：8000×8000 PNG 可解码，10000×10000 被拒；累计风险源码确认 | 增加跨图片的累计像素/字节预算，解出后按需降采样 |
| P-15 | P3 | `PluginBook/pluginBook.json` 对比 `mainwindow.cpp:228-270` | JSON 声明支持 `lit`，但 `detectFormat()` 只识别 EPUB/FB2/MOBI；`.lit`（OLE2）必然落到“无法识别”提示。 | 源码确认 | 移除 `lit` 声明或实现 OLE2/LIT 解析 |

---

## 四、构建 / 部署 / 测试可靠性

| ID | 级别 | 位置 | 问题与证据 | 验证 | 修复方向 |
|---|---|---|---|---|---|
| T-01 | P2 | `Plugins/PluginBook/test/PluginBookTest.pro:15-24` | 独立测试工程未包含 `Plugins/common` include、`zipreader.cpp` 与 `-lz`。 | 实测编译失败：`fatal error: zipreader.h: No such file or directory` | 复用 `PLUGIN_SHARED_SOURCES = zipreader`，补 include 与 zlib |
| T-02 | P2 | `Plugins/plugin_common.pri:57-60` | sidecar 复制命令尾部 `|| echo sidecar-copy-skipped` 吞掉失败，构建显示成功但 `plugins/<DLL>.json` 可能缺失，C1 兜底路由失效。 | 源码确认 | 去掉 `|| echo`，让 copy 失败导致构建失败 |
| T-03 | P3 | `Plugins/plugin_common.pri:76-80` | `meta_stamp` 的 touch 失败被 `|| echo touch-skip` 吞掉，但戳文件仍更新；只改 JSON 时 moc 不重跑，DLL 内嵌元数据与 sidecar 不一致。 | 源码确认 | touch 失败必须让目标失败，或使用 qmake 原生依赖 |
| T-04 | P2 | `tests/plugin_tests.cpp:265-269` | W1 后缀路由遇到用户 QSettings 映射就 `continue`，只写 INFO 不计断言；测试仍报 0 FAIL。 | 源码确认 | 测试前用临时 QSettings 隔离，或缺失断言时明确 FAIL |
| T-05 | P2 | `tests/plugin_tests.cpp:2092` 对比 `:918` | W13 从 `-plugins` 构建目录加载 `PluginWord.dll`，而 PluginManager 与 V 组从 `<exe>/plugins` 副本加载；同一 DLL 双路径映射，正是项目已记录的退出期虚表悬空崩溃模式。 | 源码确认 | 所有测试统一从 `<exe>/plugins` 加载 |
| T-06 | P3 | `tests/plugin_tests.cpp:1789-1801` | K3 断言包在 `if (exists(dup_paths.dat))` 内，无 else 分支；缺素材时断言整条消失而退出码仍可为 0。 | 源码确认 | 缺 fixture 时 `report(false, ...)` 或启动前校验全部素材 |
| T-07 | P2 | `tests/plugin_tests.cpp:907-911,2135` | `FW_VIEWER_ONLY` 环境变量会静默裁掉其余 V 组断言且无任何跳过记录。 | 源码确认 | 打印显式跳过清单并纳入断言总数 |
| T-08 | P2 | `tests/plugin_tests.cpp:2135-2136` | `FW_SKIP_VIEWER_SMOKE` 非空时整组 V 跳过，脚本仍报全通过。 | 源码确认 | 同 T-07 |
| T-09 | P2 | `tests/plugin_tests.cpp:2191-2196`；`run-plugin-tests.ps1:173-176` | 测试日志只在 main 末尾一次性写入；早期 return 或崩溃时脚本回显上次旧日志的 `RESULT: ... 0 FAIL`，与退出码矛盾。 | 源码确认 | 启动时先清空/写运行标记， runner 校验时间戳 |
| T-10 | P2 | `deploy/deploy-deps.ps1:34,98-100` | 脚本 `$ErrorActionPreference='Stop'`，但 `Copy-Item -Force` 无 try/catch；程序运行时 DLL 被占用会抛未捕获异常，构建/`deploydeps` 失败。 | 源码确认 | 捕获占用异常并给出明确中文提示，或跳过已锁定同版本文件 |
| T-11 | P3 | `PluginBook/PluginBook.pro:31-36`；`test/PluginBookTest.pro:31-37`；`thirdparty/libmobi/Makefile.mingw:22-33` | 两个工程共享源码树内的 `libmobi.a` 与 `.o`，并行构建会同时写同名产物，可能生成损坏静态库。 | 源码确认 | 每个影子构建目录使用独立 object/archive 输出 |
| T-12 | P3 | `tests/fixtures/make_fixtures.py:20-21,541-543,568-570,679-684`；`tests/README.md:46` | 文档只声明 `python-docx + Pillow`，实际还依赖 `openpyxl` 与 `python-pptx`；本机实测 `pptx=MISSING`。生成中途失败会留下新旧混合素材。 | 实测依赖检查 | 补全依赖声明；生成前预检全部模块，失败时写临时目录再原子替换 |
| T-13 | P3 | `run-plugin-tests.ps1:113,122,125-127`；`make_fixtures.py:36` | `-RegenerateFixtures` 忽略 `-FixturesDir`，生成脚本固定写回 `tests/fixtures`；组合参数时测试读的是另一目录的旧素材。 | 源码确认 | 把目标目录传给生成脚本，或互斥这两个参数 |
| T-14 | P3 | `tests/README.md:124`；`include/plugininterface.h:28-34` | 文档/版本史仍写“当前接口版本 = 3”，而代码 `PLUGIN_API_VERSION 4`、8 个插件 JSON 均为 4；按文档写 3 会被 W8 门禁拒绝。 | 源码确认 | 同步 README 与版本史；增加文档/代码版本一致性检查 |

---

## 五、已验证并排除的候选（不列入最终问题）

| 候选 | 排除理由 |
|---|---|
| RTF 段落对齐/缩进跨文件未复位 | 实测同一 `RtfDocument` 连续 open 两个文档，`BBB` 段落 `align=0`，未复现 |
| QDomDocument XML 实体指数展开（Billion Laughs） | Qt 6.9.3 实测拒绝：`Entity expands to more characters than the entity expansion limit`；不能列为当前代码缺陷 |
| QDomDocument 10000 层深嵌套导致解析失败 | 实测 `setContent()` 接受 10000 层；自研递归栈溢出未做实证，不列为确定问题 |
| 单张巨图 `QImage::fromData` 直接 OOM | Qt 6.9.3 `QImageReader::allocationLimit()=256MB`；8000×8000 可解码，10000×10000 被拒。仅保留“累计预算缺失”问题 P-14 |
| `filelistmodel.h` 未列入 HEADERS 导致增量构建不重编 | 生成 Makefile 中 `mainwindow.o` 依赖已自动包含 `filelistmodel.h`，误报 |
| PluginMedia 链接 avdevice/avfilter 但部署清单缺 DLL | 当前 DLL 导入表未使用这两个库，不可触发 |
| 主程序 `QTreeView` 未设置 SelectRows | 实测 `selectionBehavior=1`（SelectRows），误报 |
| `m_genId` 跨线程数据竞争 | 相关写发生在 worker 停止后、新 `doDecode` 排队前；未证实并发窗口，不列入 |
| 模型先于视图析构 | Qt QObject 析构会清理连接；未实证崩溃，不列入 |
| PluginBook/RTF 等“无深度递归”一般化风险 | 与 QDom 深嵌套候选合并处理，未实证，不列入 |

---

## 六、验证记录摘要

1. 仓库测试：构建主程序与全部插件后运行 `tests/run-plugin-tests.ps1 -SkipShots`，日志 `tests/build/logs/plugin_tests.log` 为 `RESULT: 188 PASS, 0 FAIL`。这些用例未覆盖本报告多数新增问题。
2. 树形列表探针：构造 `sub/deep/only.txt + top.txt` 归档，实测目录 `data()=bool`、文件第 1/2 列 `data()=invalid`、点击第 1 列后 `currentIndex.column()==1`、压缩节点残留空 `deep` 行。
3. 媒体探针：真实 `PluginMedia.dll` + 3 秒 WAV，实测 Stop 后按钮仍为 `⏸`，再点击时间保持 `0:00 / 0:03`。
4. 解析探针：`{\rtf1\ansi\qc A\par\u2032\'27}` 输出 3 字符并多出回退 `'`；RTF 字体名 `evil" onmouseover="x"` 成功注入 HTML；ODP `10cm` 输出 `width="10"`。
5. 构建复现：`PluginBookTest.pro` 影子构建在 `mainwindow.cpp` 编译阶段因缺少 `zipreader.h` 失败。
6. 图像与 XML 否决探针：Qt 6.9.3 分配上限 256 MiB；10000×10000 PNG 被拒、8000×8000 可解码；实体展开超过 Qt 上限被拒。
7. 最终复扫：主程序、插件、构建测试三个维度在排除全部已知项后均报告“未发现新问题”，审计收敛。

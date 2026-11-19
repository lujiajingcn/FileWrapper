# FileWrapper 第三轮全工程循环审计报告

- 审计日期：2026-11-19
- 审计基线：`HEAD = 28dbbe5`（2026-11-18），工作区在审计开始前除两份既有审计报告外干净
- 与前两轮的关系：上两轮报告（`CODE_AUDIT_FINAL_2026-11-18.md` 43 项、`CODE_AUDIT_CYCLE2_2026-11-18.md` 109 项）的问题已在 `2661188..28dbbe5` 共 **52 个提交**（audit/audit2/Batch 系列）中系统性修复。本轮是针对修复后代码树的**全新全量审计**，重点是修复引入的回归、修复未覆盖的路径、以及前两轮未细查区域。
- 范围：285 个受控源码/配置文件；58 个关键源码文件在上次审计后被改动。排除第三方（libmobi、FFmpeg headers）、生成物（ui_*.h、build/）。
- 方法：3 轮扫描（每轮主程序 / 插件 / 构建·部署·测试 三至五维并行），候选逐条读完整上下文；关键项用独立探针或只读布尔复现坐实。**第 3 轮三个维度均为"未发现新问题"，审计收敛。**
- 工具链：Qt 6.9.3 / MinGW 13.1.0；仓库测试套件 `tests/run-plugin-tests.ps1 -SkipShots`。
- 产品代码未改动；临时探针位于 `.workbuddy/tmp/`（gitignore）。

## 结论概览

最终确认 **16 项问题**：P1 1 项、P2 7 项、P3 8 项。

| 区域 | 数量 | P1 | P2 | P3 |
|---|---:|---:|---:|---:|
| 主程序（FileWrapper） | 6 | 0 | 4 | 2 |
| PluginMedia | 2 | 0 | 1 | 1 |
| 文档 / 查看器插件 | 6 | 0 | 2 | 4 |
| 构建 / 部署 / 测试 | 2 | 1 | 0 | 1 |

严重性约定：**P1** 不可逆数据丢失或核心操作失效；**P2** 可触发的功能错误、资源/性能风险；**P3** 边缘路径、生命周期清洁度、文档/配置一致性。

验证标记：**实测**＝独立探针/真实插件/只读复算复现；**源码**＝完整代码路径与触发条件确认，未做运行期峰值测量。

仓库测试基线：`194 PASS, 0 FAIL, 3 SKIP`（独立子进程运行 runner，退出码 0）。多数新发现问题未被现有断言覆盖。

---

## 一、主程序（FileWrapper）

| ID | 级别 | 位置 | 问题与证据 | 验证 | 修复方向 |
|---|---|---|---|---|---|
| C3-01 | P2 | `mainwindow.cpp:1251-1257,1363-1383`；`mainwindow.h:173-174` | `d28b3d4` 新增的大文件后台 CRC 校验结果按 `displayPath`（`info.sFilePath`）缓存进 `m_setMappedCrcVerified/Pending`，而这两个集合**全仓无任何 `clear`**（`loadArchiveFile`/`closeAllTabs`/`unLoadMergedFile` 均未清）。加载归档 A 校验通过的 `E:/vids/x.mp4`，再加载归档 B 中同名但已损坏的大文件，校验被直接跳过。若窗口析构导致回调 `self.isNull()` 提前 return，`Pending` 项还会永久残留。 | 源码（全仓 grep 确认无 clear） | 缓存键加入归档身份（归档路径＋内容 CRC）或条目 nContentPositon；切换/卸载归档时清空两个集合 |
| C3-02 | P2 | `mainwindow.cpp:1244-1248,1363-1383,1392-1397,2724-2734,1676-1680`；`mainwindow.h:158-171` | 同一 `d28b3d4` 回归的两种后果：① 后台线程按值捕获 `QSharedPointer<MappedInput>`（`checkMappedCrcAsync`），主线程 `releaseAllMappedInputs()` 仅 `clear()` 哈希，只要后台线程尚未结束，引用计数就不归零、映射不 unmap → `rewriteArchive()` 的 `QFile::rename` 在 Windows 因文件被映射而失败，保存报"被占用"。② 当后台线程成为最后持有者时，`MappedInput::~MappedInput()` 在 detached 线程执行 `pFile->unmap()` + `delete pFile`，而 `pFile` 是主线程 `new QFile`（`:1235`）——跨线程销毁 QObject，违反线程亲和性。提交说明只解决了 UAF，未更新保存/析构契约。 | 源码 | 让 CRC 校验完成回调（主线程）统一释放映射；保存前等待进行中的校验结束（带超时与明确提示），再 unmap |
| C3-03 | P2 | `mainwindow.cpp:1790-1813,1879-1925` | `72bf3fa` 把目录扫描移到进度框显示之后，声称使"取消"可用；但 `FindFile::walk`（1888-1923）全程**无 `processEvents()`**，仅检查 `isCancelRequested()`。而取消信号 `canceled → cancelOperation` 置位依赖事件循环派发鼠标事件。扫描超大目录时主线程深陷无事件循环的递归，取消按钮"看得见点不动"。 | 源码（两维度独立发现） | 在 `walk` 的循环内定期 `QApplication::processEvents()`，或把扫描移入工作线程并用信号回报进度 |
| C3-04 | P2 | `filemanager.cpp:983-1003`（对比 `:1552`） | 合并时"重名自动改名"循环从 `n=1` 重新试探，对第 k 个重名要试探 k 次，N 个同名条目为 O(N²)；循环内**无 `n` 上限、无 `processEvents()`、无取消检查**。分割/导出的同类 `chooseOutputPath` 有 `n>100000` 硬上限。合并上千个各含根级同名文件的 `.dat` 时，进度框在去重阶段卡死且取消无效。 | 源码 | 复用带上限的 `chooseOutputPath`；循环内泵事件并响应取消 |
| C3-05 | P3 | `mainwindow.cpp:186,2566-2580`（对比 `:454`） | `m_setExpandedTree` 存的是相对目录文本链（如 `"dir/sub"`），与具体归档无关，仅在视图切换时 clear/restore；`loadArchiveFile`/`closeAllTabs` 未清。归档 A 展开 `a/b` → 切平铺 → 加载含 `a/b` 的归档 B → 切树形，B 的 `a/b` 被无端自动展开。与 C3-01 同源但是不同集合、不同触发点。 | 源码 | 加载/卸载归档时清空该集合，或把展开状态按归档路径分组保存 |
| C3-06 | P3 | `mainwindow.cpp:1357-1358` | `renderFile` 中唯一漏掉内容区复位的失败出口：内存通道 `sendFile()` 返回 false 时只弹状态栏警告，未 `sendFileData(nullptr,0)`；同函数其它失败分支（1300/1307/1325/1349）均复位。**可达性低**：现有插件的 `showFile()` 仅在数据为空时返回 false，而此处数据已判非空；Media 自身失败也会 `clearContent()`。仅第三方插件对非空数据返回 false 时暴露。 | 源码（低可达性） | 与其它失败分支一致，补 `iface->sendFileData(nullptr,0)` + `syncActionStates()` |

---

## 二、PluginMedia

| ID | 级别 | 位置 | 问题与证据 | 验证 | 修复方向 |
|---|---|---|---|---|---|
| C3-07 | P2 | `mainwindow.cpp:454-470`（正常帧）对比 `:340-350`（EOF drain）、`:96-98,1836-1838` | `srcW/srcH/m_srcPixFmt` 只在 EOF 的 `drainVideo` lambda 内随实际帧更新；正常帧循环直接用打开时 `pCodecCtx` 的值做 `sws_scale`。`38d3476` 为 M-05 补帧时只在 drain 路径加了同步，正常路径未改。播放中分辨率/像素格式变化的媒体（TS 自适应、拼接片段）会用旧 `srcH`/旧源格式读取 `pFrame->data` → 越界读/花屏；若打开时 `pix_fmt==AV_PIX_FMT_NONE`，`rebuildVideoSws` 恒早退、`m_sws` 为 null，正常路径每帧 `continue`，整段无画面直到 EOF 才补尾部几帧。 | 源码 | 在正常帧分支每帧检查 `pFrame->width/height/format`，变化时重建 sws（与 drain 共用同一逻辑） |
| C3-08 | P3 | `pluginmedia.cpp:65-72`（对比 `:33-38,79-83`）、`mainwindow.cpp:987-1042` | `de782b2` 修 V-03 时给 `showFile`/`sendFileMapped` 的失败路径补了 `clearContent()`，**漏掉同批次的 `sendFileByPath`**：路径通道打开失败只 `return false`，不复位。进度条、时间标签、播放图标保留上一文件状态；`clearFFmpegResources()` 已把 `m_bPlaying` 置 false 却不再刷新图标 → 死控件。宿主当前在路径失败后回退内存通道可能掩盖此现象，直接调用该接口必现。 | 源码 | `sendFileByPath` 失败时调用 `clearContent()`，与其它两条通道一致 |

---

## 三、文档 / 查看器插件

| ID | 级别 | 位置 | 问题与证据 | 验证 | 修复方向 |
|---|---|---|---|---|---|
| C3-09 | P2 | `PluginSlides/slidesdocument.cpp:65-75`（`parseRels`，调用点 195/243）；`PluginSheet/sheetdocument.cpp:103-114`（调用点 231） | D-01 修复（`b65d9e6`）为防 QDom 节点放大，给所有 `setContent` 加了 `xmlWithinTagBudget`，唯独 3 处 `parseRels` 遗漏。构造数十万个 `<Relationship/>` 的超大 rels 条目可绕过全部防护，在 GUI 线程物化大量 QDom 节点造成内存/卡顿。 | 源码（grep 对比 10 处调用全避开 parseRels） | `parseRels` 内在 `setContent` 前调用 `xmlWithinTagBudget(relsXml)` |
| C3-10 | P2 | `PluginSlides/slidesdocument.cpp:288-317`（`findBlip`，append 在 310）、`:461-476`（ODP draw:image，append 在 476） | D-34 修复（`1eddbb3`）只在 `a:p`/`text:p` 文本分支加了 `kMaxBlocks` 检查（353/502），图片分支的 `slide.blocks.append()` 无上限。缓存命中时图片不重复解码、不消耗预算，但每引用一次就 append 一个 Block；单个 `p:pic` 放上万 `<a:blip>` 可让单页积累远超 2000 的 block，render 生成同量级 `<img>`，绕过全部块预算。 | 源码 | 图片 append 前同样检查 `kMaxBlocks` 并置 `m_truncated` |
| C3-11 | P3 | `Plugins/PluginPicture/mainwindow.cpp:49`（检查）对比 `:64`（转换） | D-32 修复（`5569738`）把长度上限改为 `uint::max()`（≈4.29e9）判断，却把缓冲区长度写成 `static_cast<int>`（上限 2.15e9）。nFileLen∈(2GiB,4GiB] 时通过检查但转 `int` 变负，`QBuffer::setData(char*, qsizetype)` 收到负值 → 断言/UB。 | **实测**（算术复算：3e9→-1294967296） | 统一用 `qsizetype`/`qint64` 且上限取 `INT_MAX`，或拒绝 >2GiB |
| C3-12 | P3 | `PluginWord/worddocument.cpp:2144-2146`；`PluginSlides/slidesdocument.cpp:461-466`（对比 `PluginBook/mainwindow.cpp:35`） | D-08 已为 EPUB 加了 `QUrl::fromPercentEncoding`，但 ODT/ODP 的 `xlink:href` 未同步。ODF 规范中 href 是 IRI，含空格/非 ASCII 的图片名写成 `Pictures/%E5%9B%BE%201.png`，而 ZIP 条目名是解码后的字面值 → 查找落空，合法文档图片静默丢失。`zipreader::normalize()` 不做百分号解码。 | 源码 | 两处 href 在查 `m_zip.entry()` 前做 `QUrl::fromPercentEncoding` |
| C3-13 | P3 | `PluginSheet/sheetdocument.cpp:270-273`（对比 `PluginWord/worddocument.cpp:398-405`、`PluginSlides/slidesdocument.cpp:46-51`） | D-39 已让 Word/Slides 正确处理 OPC 包根绝对路径（`/custom/s...`），Sheet 仍沿用旧逻辑：先剥前导 `/`，再无条件补 `xl/` → `/custom/sheets/s1.xml` 变成 `xl/custom/sheets/s1.xml`，查不到 → 整张工作表静默丢失。 | 源码 | Sheet 解析 Target 时区分绝对/相对，绝对路径不再拼 `xl/` |
| C3-14 | P3 | `PluginWord/worddocument.cpp:389-414,1421`（docx）；`PluginSlides/slidesdocument.cpp:46-61,108-111,217,295`（pptx）；`PluginSheet/sheetdocument.cpp:270-274,285`（xlsx） | 同一家族：三个 OOXML 插件的 OPC 关系部件 `Target` 均未做百分号解码。OPC 规范中部件名含空格/非 ASCII 时 Target 写成 `media/a%20b.png`，而 ZIP 条目名是字面 `word/media/a b.png`（或 `slides/slide 1.xml`、`sheet 1.xml`）→ `m_zip.entry()` 落空，图片/整页/整表静默丢失。EPUB（D-08）与本轮 ODT/ODP（C3-12）同类问题均已修，唯独 OPC 系列三条路径从未覆盖；D-39 只修了前导 `/` 绝对路径，与百分号编码无关。 | **实测**：`space_img.docx`（Target=`media/a%20b.png`、条目名 `word/media/a b.png`）→ `stats.images=0`；纯 ASCII 对照样本 → `stats.images=1`。真实 PluginWord 代码路径复现 | 三处 Target 在 `resolvePart`/`joinPath`/补 `xl/` 之前统一 `QUrl::fromPercentEncoding` |

---

## 四、构建 / 部署 / 测试

| ID | 级别 | 位置 | 问题与证据 | 验证 | 修复方向 |
|---|---|---|---|---|---|
| C3-15 | **P1** | `FileWrapper/deploy/package.ps1:50-70`（守卫 55-61，删除 69） | 脚本头注释承诺"OutDir 等于或是 BuildDir/仓库根祖先时拒绝"，但代码只挡 `outFull==buildFull`、`outFull==盘根`、以及 `protected.StartsWith(outFull+'\')`。**缺少 `outFull==repoRoot` 判断**，且 StartsWith 因要求尾随 `\`，相等时恒为 false。当 Qt Creator 默认把构建树放在仓库外（兄弟目录）而 `-OutDir` 指向仓库根并带 `-Overwrite`/标记文件时，三守卫全放行 → `Remove-Item -LiteralPath $OutDir -Recurse -Force` 递归删除整个仓库。 | **实测**（只读布尔复算：build 在仓库外兄弟目录、OutDir=repoRoot 时 guard1/2/3 全 False、`ALL_GUARDS_PASS_TO_DELETE=True`） | 增加 `$outFull -ieq $repoRoot` 判断；祖先检查用规范化路径＋分隔符双重判定；删除前打印目标并要求确认 |
| C3-16 | P3 | `tests/README.md:125`；`include/plugininterface.h:37`；8 个 `pluginXxx.json` | Batch 33 将 `PLUGIN_API_VERSION` 升到 5（新增 `stopPlaybackAndWait`），README 仍写"当前接口版本 = 4（v4 增加 sendFileMapped）"，版本史顺序也乱。这是上轮 T-14（文档写 3、代码 4）修复后再次漂移。按文档给新插件 JSON 写 `apiVersion:4` 会被 W6 计为 bad sidecar、被 W8a 门禁拒绝加载。 | 源码（README:125 vs 代码 37，8 个 JSON 均为 5） | 同步 README 为版本 5；增加文档/代码/JSON 版本一致性检查用例 |

---

## 五、已排除的候选（不列入最终问题）

| 候选 | 排除理由 |
|---|---|
| runner `EXITCODE=1` 且无 RESULT 日志（C3-T01/C3-U02，两维度报告） | **实测否决**：这是我用 `powershell ... 2>&1 \| Out-File` 包裹调用造成的假象——PowerShell 5.1 在 `2>&1` 重定向下把测试程序写 stderr 的 `[INFO]` 当终止错误。用独立子进程（`Start-Process -RedirectStandard*`，模拟用户真实命令）运行 runner，`EXITCODE=0`、`RESULT: 194 PASS, 0 FAIL, 3 SKIP`。测试程序本身直接运行也为 194 PASS / exit 0。runner 与测试程序均无此 bug。 |
| `renderFile` 中 `sendFile()` 返回 false 不复位（C3b-M02） | 归为 C3-06，但可达性低：现有所有插件对非空数据都不会返回 false，且 Media 自行复位。保留为 P3，不作为独立功能 bug。 |
| Media 连续快速 seek 的 genId 竞态（C3-V03） | 时序窗口为亚毫秒、手动 UI 难稳定触发，未实证，列风险不列确定问题。 |
| Media `stopPlaybackAndWait()` 30s 超时不停 timer/音频 | 需 worker 卡死 >30s 才可达，可达性极低；已 documented 为残余风险（前两轮 M-06 同类）。 |
| RTF `m_ucStack` 多顶级组状态泄漏（C3-D06） | 需畸形多顶级组 RTF，标准文档 `reset()` 清空无可见影响；未实测，列待验证。 |
| 顶层 `deploydeps` 默认 qmake 配置只转发 Debug（C3-V01） | 7e3d27f 已修显式 CONFIG 路径；默认 `debug_and_release` 路径未实测，列风险。 |
| `FW_VIEWER_ONLY`/`FW_SKIP_VIEWER_SMOKE` 环境变量裁剪、`toHtml().contains("<table")`、`toHtml` 恒真等测试项 | 前轮已用 `g_skipped` 显式计数覆盖；`<table` 经核对 Qt 6 仅在确有表格时输出，非恒真。 |
| avdevice/avfilter DLL 未入部署清单 | 插件导入表未使用这两个库，不可触发（与前两轮结论一致）。 |
| 单张巨图 OOM / QDom 实体指数展开 / QDom 深嵌套 / filelistmodel.h 增量依赖 | 前两轮已实测否决（Qt allocationLimit 256MB、Qt entity expansion limit、生成 Makefile 已含头依赖），本轮不重复。 |

---

## 六、验证记录摘要

1. **基线测试**：独立子进程运行 `tests/run-plugin-tests.ps1 -SkipProjectBuild -SkipShots` → `EXITCODE=0`、`RESULT: 194 PASS, 0 FAIL, 3 SKIP`；直接运行测试程序（runner 相同参数）结果一致。确认 runner 与测试程序均正常，排除两维度误报的"退出码 1"问题。
2. **CRC 生命周期**：全仓 grep 确认 `m_setMappedCrcVerified/Pending` 无任何 `clear`（`mainwindow.cpp:246/473/886` 处的 clear 均为其它集合）；`MappedInput::~MappedInput` 跨线程 delete 路径由 `checkMappedCrcAsync` 的按值捕获 `QSharedPointer` 推出。
3. **Picture 溢出**：算术复算 `static_cast<int>(3e9) = -1294967296`，确认 (2GiB,4GiB] 区间通过 `uint::max()` 检查但转 `int` 为负。
4. **OPC 百分号解码**：构造最小 docx（`word/media/a b.png` + `Target="media/a%20b.png"`），用真实 `WordDocument::open/render` 探针实测 `stats.images=0`；纯 ASCII 对照样本 `stats.images=1`。坐实图片静默丢失。
5. **package.ps1 删库**：以只读 PowerShell 复算守卫布尔——build 树在仓库外兄弟目录、`OutDir=repoRoot` 时 guard1/2/3 全 False，`ALL_GUARDS_PASS_TO_DELETE=True`。坐实守卫缺口（未实际执行删除）。
6. **收敛**：第 3 轮主程序、插件、构建测试三个维度在排除全部已知项后均报告"未发现新问题"，审计停止。

---

*本报告仅审计与文档，未修改产品代码。所有行号以 HEAD `28dbbe5` 为准。工作区中出现的 `FileWrapper/dlgmergefile.ui`、`resources/icons/unload*.png`、`_eval_merge_buttons/` 等改动非本次审计产生，未纳入审计结论。*

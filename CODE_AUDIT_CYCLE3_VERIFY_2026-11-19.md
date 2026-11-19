# FileWrapper 第三轮审计报告核对

- 核对日期：2026-11-19
- 核对对象：`CODE_AUDIT_CYCLE3_2026-11-19.md`（下称"原报告"）第一节列出的 16 项确认问题
- 基线：`HEAD = 28dbbe5`（2026-11-18）；工作区源码与核对开始时一致，本次核对**未修改任何产品代码**
- 方法：逐条读完整代码上下文 + 全仓 grep 交叉验证；关键项用只读布尔复算、或调用真实插件代码路径的探针复现
- 范围：只核对 16 项确认问题。原报告第五节的"已排除候选"与测试基线（194 PASS / 3 SKIP）不在本次范围

## 结论概览

**16 项全部真实存在，无虚构项。** 结论层面零误报；表述层面需要修正：其中 **C3-07 的"drain 已同步"与 C3-01 的"析构残留 Pending"是核心论证错误**，**C3-14 与原报告 C3-12 自相矛盾**，C3-11 的提交归因与可达性、C3-09 的修复建议、C3-13 的"无条件补 `xl/`"、C3-03 的"取消无效"需要更正。

| 判定 | 数量 | 编号 |
|---|---:|---|
| 真实存在，按原报告机制可触发 | 10 | C3-02、C3-03、C3-04、C3-08、C3-09、C3-10、C3-12、C3-13、C3-14、C3-16 |
| 真实存在，但需特定条件 | 4 | C3-01、C3-05、C3-07、C3-15 |
| 真实存在，但生产路径不可见 / 不可达 | 2 | C3-06、C3-11 |

严重性定级总体合理：`C3-15` 的 P1 可辩护（真实数据丢失路径，但需显式误用参数）；`C3-11` 的 P3 正确（产品内不可达）；`C3-07`、`C3-09`、`C3-10` 的 P2 合理。

## 一、主程序（FileWrapper）

### C3-01｜CRC 校验缓存不随归档清空

- **核对结论**：核心成立；"窗口析构导致 Pending 永久残留"的子论断**错误**。
- **证据**：`mainwindow.h:173-174` 声明两个 `QSet<QString>`；`mainwindow.cpp:1251-1257` 用 `info.sFilePath` 查两个集合并插入 Pending；`:1375-1377` 回调里 `remove(Pending)`、成功才 `insert(Verified)`；全仓只有 contains/insert/remove，**无任何 `clear`**；`closeAllTabs`（`:454-482`）与 `loadArchiveFile`（`:1727-1773`）都不清。
- **偏差**：
  1. `self.isNull()` 只可能发生在 `MainWindow` 已析构时，而两个集合是它的成员，会随对象一起销毁，不存在"永久残留"。
  2. 影响面仅限 **≥128MB 且走 `sendFileMapped`** 的路径（`kLargeFileThreshold = 128MB`，`mainwindow.cpp:53`）；内存通道每次都校验 CRC，保存时也会比对，损坏文件仍会在保存阶段暴露。
  3. 跨归档碰撞要求两个归档的 `displayPath` 完全相同（v2 归档 = 原始根 + 相对路径；v1 = 归档内原路径）。"同源重合并"工作流下成立。

### C3-02｜后台 CRC 线程延迟 unmap / 跨线程销毁 QFile

- **核对结论**：**成立**。
- **证据**：`mainwindow.h:158-171` 的 `~MappedInput()` 执行 `pFile->unmap(pData)` + `delete pFile`；`mainwindow.cpp:1235` `new QFile(...)` 在主线程；`:1363-1382` `std::thread([self, sPath, mapped, nExpected](){...}).detach()` **按值**捕获 `QSharedPointer`；`:1392-1397` `releaseAllMappedInputs()` 只 `m_mapMappedInputs.clear()`；`filemanager.cpp:1474-1479` 保存时 `m_qFile.close()` 后立即 `QFile::rename`。
- **只读探针**（本机 Qt 6.9.3）：文件被普通 `QFile` 打开时 `QFile::rename` 返回 false、`GetLastError()=32`（ERROR_SHARING_VIOLATION）；已 map 时同样 32；关闭句柄后成功。
- **偏差**：
  1. ①是**竞态**而非必现——保存前 `mainwindow.cpp:2724-2733` / `1676-1680` 已先 stop + release，唯一残留持有者是尚未结束的 CRC 线程。
  2. 归因应补上"打开的 `QFile` 句柄"：不等 unmap，仅句柄未关就足以让 rename 失败。
  3. ②排除：哈希先摘时，`unmap` 与 `delete QFile` 确定在 worker 线程执行，无 join/等待，属明确的 QObject 线程归属违规。

### C3-03｜扫描期间进度框无法取消

- **核对结论**：**成立**，但"取消完全无效"夸大。
- **证据**：`mainwindow.cpp:1791-1801` 建 `QProgressDialog`、`beginProgressOperation`、`show()` 后只 `processEvents()` 一次；`:1813` 同步调用 `FindFile`；`:1888-1922` 的 `walk` 全程无 `processEvents()`，只在入口和循环里轮询 `isCancelRequested()`；取消信号在 `:2428-2429` 连接，`FileManager::cancelOperation` 仅置位（`filemanager.cpp:1503-1505`），置位依赖事件循环派发鼠标事件。
- **偏差**：点击事件只是**排队**；扫描结束后，取消会在合并复制的 `filemanager.cpp:788` `processEvents()` 时被派发，合并随即中止。准确说法是"扫描阶段不可取消、取消延迟到合并开始后才生效"，而非永久失效。提交说明"扫描阶段可点取消"与实现不符，这一点成立。

### C3-04｜合并重名循环 O(N²) 且无上限 / 无事件泵

- **核对结论**：**成立**（算法层）。
- **证据**：`filemanager.cpp:987-999` 从 `n=1` 重新试探，第 k 个重名试 k 次；循环内无 `n` 上限、无 `processEvents()`、无取消检查；对照 `:1547-1552` 的 `chooseOutputPath` 有 `n > 100000` 上限。
- **偏差**：`chooseOutputPath` 只被分割功能使用；导出用的是 `outputFile` 自己的 `:2030-2034` `n < 100000` 循环。O(N²) 只对同一 `sStoredPath` 分组成立；`DlgMergeFile::duplicatCheck` 会拒绝完全重复的输入字符串，但"目录 + 目录内文件 / 嵌套目录 / 自制归档"仍可放大，可达但偏构造。

### C3-05｜树形展开状态跨归档残留

- **核对结论**：**成立**。
- **证据**：`mainwindow.h:186` 声明 `QSet<QString> m_setExpandedTree`；键在 `mainwindow.cpp:2491-2495` 由"根节点全文 + `/` + 目录名"拼成；恢复在 `:2511-2512`；唯一 `clear()` 在 `:2578`（切平铺时）；`closeAllTabs`、`loadArchiveFile` 均不清。
- **偏差**：
  1. 行号错：报告写 `mainwindow.cpp:186`，实际声明在 `mainwindow.h:186`。
  2. 键不是"相对目录文本链"而是**含根前缀的路径文本链**（如 `E:/root/a/b`），两个归档公共根不同时不会碰撞，报告示例需增加"同根前缀"条件。
  3. 报告未提：`populateFilePathTree` 在 `:2178-2181` 先采集旧模型的展开键到局部 `setExpanded`，重建后在 `:2266-2277` 恢复——**树形模式下直接切换归档同样会串展开状态**。只清成员集合修不干净，须同时处理这个局部集合。

### C3-06｜内存通道 `sendFile()` 失败未复位内容区

- **核对结论**：代码缺口**成立**，但用户可见后果在当前插件下不成立。
- **证据**：`mainwindow.cpp:1357-1358` 失败只 `onFileManagerWarning(...)`；同函数其余失败出口（1300/1307/1325/1349）都 `iface->sendFileData(nullptr, 0)`；`Plugins/common/pluginbase.h:64-68` 对空数据自清；唯一能在非空数据返回 false 的 `PluginMedia` 在返回前已 `clearContent()`（`pluginmedia.cpp:34-38`）。
- **偏差**：报告暗示的"残留上一个文件内容"在现有插件下不会发生，属面向未来/第三方插件的**契约缺口**。原报告自评"可达性低"方向正确，应进一步说明"可达但被插件侧 `clearContent()` 掩盖"。

## 二、PluginMedia / PluginPicture

> 原报告把 C3-11 归在"文档 / 查看器插件"一节，但代码位于 `Plugins/PluginPicture`；本节按模块归属把它与 PluginMedia 放在一起，编号保持不变。

### C3-07｜正常帧路径不刷新 `srcW/srcH/m_srcPixFmt`

- **核对结论**：核心**成立**；但"drain 已修好"的对照**不成立**。
- **证据**：`PluginMedia/mainwindow.cpp:96-98` 打开时写入一次；正常帧 `:450-470` 只在**输出尺寸请求变化**时用缓存的 `srcW/srcH/m_srcPixFmt` 重建，`:468-470` 用缓存的 `srcH` 调 `sws_scale`；`:344-346` 是全文件唯一后续更新点（EOF drain）；`git show 38d3476` 确认只新增了 drain 段。
- **偏差**：
  1. **drain 的重建是死调用**：`:347-349` 传入的 `newW/newH` 就是当前 `outW/outH`，而 `rebuildVideoSws` 的复用条件 `:150-152` 只比较输出尺寸，`m_sws` 非空时必然 `return`，旧上下文从不替换；随后 `:352-353` 反而用**新的** `srcH` 去配旧 sws。报告把它当作"已修好的对照实现"不成立，其"照抄 drain 逻辑"的修复建议也会被这个早退挡掉。
  2. `pix_fmt == NONE` 时不是"EOF 才补尾部几帧"：`rebuildVideoSws` 在 `:147-148` 早退，`m_sws` 恒 null，`outW/outH` 只在重建成功时写入（`:185-186`）→ 始终为 0 → EOF 重建在 `:143-144` 再早退，**整段永久无画面**。
  3. `pix_fmt==NONE` 的可达性未实证（`avformat_find_stream_info` 通常会填上）；另有一个同源变体：打开时 `width/height==0` 也会走同一条永久黑屏路径。
  4. "越界读"需分方向：新帧变矮/变窄/位深变低才会读过 plane 末尾；变大时只是裁切/花屏。

### C3-08｜`sendFileByPath` 失败不复位 UI

- **核对结论**：**成立**（三条通道不对称）。
- **证据**：`pluginmedia.cpp:33-38`（showFile）、`:79-83`（sendFileMapped）失败均 `clearContent()`；`:65-72`（sendFileByPath）失败只 `return false`；`clearContent` 复位进度条/时间/图标（`PluginMedia/mainwindow.cpp:938-960`）；`clearFFmpegResources` 在 `:1033` 置 `m_bPlaying = false`，但整个函数无 `updatePlayPauseIcon()`。
- **偏差**：
  1. 宿主回退是**确定性**掩盖：路径通道失败后 `FileWrapper/mainwindow.cpp:1287-1294` 回退内存通道，两条通道失败都会复位；只有直接调用该接口（或未来改回退策略）才必现，且需"路径存在但媒体打不开"。
  2. 画面并**没有**残留：`showVideo` 在 `:1481-1484` 已 `clearVideoPicture()`；残留的是进度条范围/值、时间标签、播放图标。
  3. 不存在后台继续播放：`clearFFmpegResources` 已停 timer、删音频输出、释放 FFmpeg 上下文；现象是 UI 状态陈旧 + 播放按钮无效。

### C3-11｜`uint` 上限检查与 `static_cast<int>` 窄化

- **核对结论**：类型不一致**成立**，但产品内**不可达**，归因错误。
- **证据**：`PluginPicture/mainwindow.cpp:49` `if (nFileLen > qint64(std::numeric_limits<uint>::max()))`，`:64` `buf.setData(szBuf, static_cast<int>(nFileLen))`；`QBuffer::setData` 实际形参是 `qsizetype`；宿主 `FileWrapper/mainwindow.cpp:53` `kLargeFileThreshold = 128MB`，`:1319-1327` 对非 `prefersFileByPath` 插件直接拒绝，`PluginPicture` 未 override 该函数。
- **偏差**：
  1. 归因错：`uint::max()` 判断来自 `f871a13`（2026-11-15，配 `QPixmap::loadFromData`），不是 D-32/`5569738`；`5569738` 引入的是 `static_cast<int>` 窄化，同时留下与 `QBuffer` 实际 API 不符的旧注释。
  2. 报告"实测"只是算术复算（3e9 → -1294967296），未验证可达性；128MB 闸门使 `(2GiB,4GiB]` 在产品路径上不可能出现，属**接口级潜伏缺陷**。
  3. 修复方向应改为直接传 `qsizetype(nFileLen)`，而不是"上限取 `INT_MAX`"（那只是把窄化前移）。

## 三、文档 / 查看器插件

### C3-09｜`parseRels` 绕过 QDom 标签预算

- **核对结论**：**成立**，且已实测复现。
- **证据**：`xmlguard.h:8-18` 的预算为 64 MiB + 500k 标签；`slidesdocument.cpp:65-69`、`sheetdocument.cpp:103-107` 的 `parseRels` 直接 `setContent`，调用点 `slidesdocument.cpp:194-195` / `:241-243`、`sheetdocument.cpp:230-231`；全仓其余 `setContent` 均先做 `xmlWithinTagBudget`。
- **实测**：3.49 MB pptx（rels 35.5 MB、700,003 个 `<Relationship/>`）→ 解析成功，峰值工作集约 2.13 GB、耗时 2.8 s，全程 GUI 线程；对照：520k 标签的 slide XML 被预算拒绝。
- **偏差**：
  1. "3 处 `parseRels`"实为 **2 个函数、3 个调用点**（原报告括号内已列出，不影响结论）。
  2. ZIP 层不是兜底：`zipreader.cpp` 的 64 MiB 上限只查 deflate，`stored` 条目不查。
  3. **修复建议不足**：只加 500k 标签预算仍可在预算内打出约 1.5 GB（QDom 放大主要来自**属性**；实测 400k 带属性节点即 1.14 GB）。应按 Relationship 条数/rels 字节数单独限流（如 ≤1 万条）。

### C3-10｜图片分支的 block 无上限

- **核对结论**：**成立**，且已实测复现。
- **证据**：`slidesdocument.cpp:14` `kMaxBlocks = 2000`；文本分支 `:353`、`:502` 有检查；图片分支 `:288-317` 的 `slide.blocks.append(b)`（`:310`）无检查；ODP 分支 `:461-476` 同形；缓存命中在 `:121-133` 直接复用 `imageUrl`，不重复解码也不消耗预算。
- **实测**：单个 `p:pic` 内 5000 个同 rId `<a:blip>` → `imgs=1`、HTML 中 5000 个 `<img>`、`truncated=0`；ODP 单 `draw:frame` 内 3000 个 `draw:image` 同理。
- **偏差**：
  1. 不是"完全无上限"：外层 `:247` 仍把同一页多形状总量卡在 2000，但单个 `p:pic` 内可累到 2000+N；`kMaxSlides=200` 使整份文档可达 200×N 个 `<img>`（报告只写"单页"，低估）。
  2. 单个 `p:pic` 内多个 `a:blip` 违反 DrawingML schema，但 `QDomDocument` 不做 schema 校验、`findBlip` 只按 tag 名递归，能解析——"非法但可解析"。
  3. 报告漏了每次仍重新 `m_zip.entry()` 解压同一图片（5000 次引用 = 5000 次 inflate）。

### C3-12｜ODT/ODP 的 `xlink:href` 未做百分号解码

- **核对结论**：**成立**，已实测。
- **证据**：ODT `worddocument.cpp:2144-2147` 取 `xlink:href` 后只去 `./`、判 `://`；查表 `:2163-2167`，失败后仅有 `Pictures/` 前缀兜底；ODP `slidesdocument.cpp:461-466` 无兜底；`zipreader.cpp:21-29` `normalize()` 只归一化斜杠；EPUB 对照 `PluginBook/mainwindow.cpp:34-35` 有 `QUrl::fromPercentEncoding`。
- **实测**：`Pictures/%E5%9B%BE%201.png` + 条目 `Pictures/图 1.png` → ODT `stats.images=0`、ODP `imgs=0`；纯 ASCII 对照 → 1。
- **偏差**：无真实文档触发证据（触发频率未证实）；ODT 有 `Pictures/` 兜底而 ODP 没有，两处形态不同；本项目不写 ODT/ODP，纯属读取健壮性。

### C3-13｜Sheet 的 OPC 包根绝对路径处理缺失

- **核对结论**：**成立**，已实测。
- **证据**：`sheetdocument.cpp:270-274` 先剥前导 `/`，再在不以 `xl/` 开头时补 `xl/`；`:285-286` 查不到就 `continue`。对照：Word `worddocument.cpp:398-405` 有 `bAbsolute` 分支；Slides `slidesdocument.cpp:46-51` 有 `rel.startsWith('/')` 分支。
- **实测**：Target `/custom/sheets/s1.xml`（条目 `custom/sheets/s1.xml`）→ `open=0`、`sheets=0`；`/xl/...` 与相对路径均正常。
- **偏差**：
  1. "无条件补 `xl/`"不准确：`:272` 有 `!startsWith("xl/")` 条件，`/xl/...` 能命中；对报告举的 `/custom/...` 结果一致。
  2. 单工作表时不是"静默丢一张"，而是整份 `open()` 失败（错误文案"没有可显示的工作表内容"掩盖真实原因）；多表时才是静默少一张且不置 `m_truncated`。
  3. 报告未提：Sheet 还缺 `./`、`..` 消解，`../custom/s.xml` 会变成 `xl/../custom/s.xml`。

### C3-14｜三个 OOXML 插件的 OPC `Target` 未做百分号解码

- **核对结论**：**成立**，已实测复现；但原报告本条与 C3-12 **自相矛盾**。
- **证据**：Word `worddocument.cpp:789-795` 原样入表、`:1419-1421` 消费，`resolvePart`（`:398-406`）只处理绝对路径与 `.`/`..`；Slides `slidesdocument.cpp:70-73`、`joinPath`（`:46-51`）；Sheet `sheetdocument.cpp:108-111`、`:270-273`；`zipreader.cpp:77` 条目名保留字面值，`:21-29` 不解码。
- **实测**：`space_img.docx`（Target `media/a%20b.png`、条目 `word/media/a b.png`）→ `stats.images=0`；ASCII 对照 → 1。另测包根绝对路径 `/custom/a.png` 在 Word/Slides 均正常出图，确认 D-39 覆盖范围。
- **偏差**：
  1. **内部矛盾**：原报告第 61 行称"EPUB（D-08）与本轮 ODT/ODP（C3-12）同类问题均已修"，而 C3-12 本身在说 ODT/ODP **未修**；实测确认未修。正确表述是"EPUB 已修，ODT/ODP 见 C3-12 未修，OPC 三条路径从未覆盖"。
  2. "ZIP 条目名是字面值"不是规范强制；MS Office 确实会解码 ASCII `%HH`，所以"Target 编码 + ZIP 字面"的包现实存在，但报告的构造样本只能证明代码行为，不能证明这是 Word 生成的常见形态。
  3. 三条路径形态不同（Word `resolvePart`、Slides `joinPath`、Sheet 只剥 `/` 补 `xl/`），统一修复要落在三处不同位置；`stats.images` 只有 Word 有，不能外推到三个插件。

## 四、构建 / 部署 / 测试

### C3-15｜`package.ps1` 缺少 `OutDir == 仓库根` 守卫（P1）

- **核对结论**：**成立**。
- **证据**：`FileWrapper/deploy/package.ps1:55-61` 三个守卫（等构建目录、等盘根、作为祖先）；`:50-53` 计算 `outFull/buildFull/repoRoot`；`:69` `Remove-Item -LiteralPath $OutDir -Recurse -Force`；`:66-68` 非空目录需 `-Overwrite` 或 `.filewrapper-package` 标记。
- **只读复算**：`OutDir = 仓库根`、构建树在仓库外兄弟目录（本机确有此形态）时 guard1/guard2/guard3 全 False → 走到删除；`OutDir` 为仓库真祖先、或构建树在仓库内时，guard3 为 True（会拒绝）。`repoRoot` 解析结果与 `git rev-parse --show-toplevel` 一致。
- **偏差**：实际触发需用户显式把 `-OutDir` 指到仓库根并加 `-Overwrite`（或仓库根恰有标记文件）。P1 定级可辩护——后果是整仓（含 `.git`）不可逆删除，且脚本头注释明确承诺拒绝"等于仓库根"。

### C3-16｜`PLUGIN_API_VERSION` 与文档漂移

- **核对结论**：**成立**。
- **证据**：`include/plugininterface.h:37` `#define PLUGIN_API_VERSION 5`；`tests/README.md:125` "当前接口版本 = 4"；8 个 `pluginXxx.json` 全为 5；`pluginmanager.cpp:116` 要求 `found == PLUGIN_API_VERSION`（精确相等）；`plugininterface.h:28-35` 版本史顺序为 3 → 5 → 4，确实乱。
- **偏差**：措辞微调——拒绝加载的是加载门禁 `PluginManager::apiVersionAcceptable`，W8 系列只是守护该门禁的测试用例；按文档写 `apiVersion:4` 会被 W6 计为 bad sidecar，并在加载时被门禁拒绝，这一点成立。

## 五、原报告需重点更正的六处

1. **C3-14 与 C3-12 自相矛盾（最该改）**：C3-14 说 ODT/ODP 已修，C3-12 说没修；实测为**没修**。应改为"EPUB 已修，ODT/ODP 见 C3-12，OPC 三条路径从未覆盖"。

2. **C3-07 的"drain 已同步"不成立**：`rebuildVideoSws` 的复用条件只比输出尺寸，drain 传入的正是当前输出尺寸，必然早退，旧 sws 从不重建；随后反而用新的 `srcH` 配旧上下文。`pix_fmt==NONE` 时 EOF 也补不出画面。报告"照抄 drain"的修复建议无效，必须把源宽高/源格式纳入复用条件。

3. **C3-01 的"析构残留 Pending"错误**：`self.isNull()` 只会在 `MainWindow` 已析构时发生，两个集合是成员，会一起销毁；真正的问题是"同一窗口内、同一 `displayPath` 不再复检"。

4. **C3-11 的归因与可达性都错**：`uint::max()` 判断来自 `f871a13`，不是 `5569738`；宿主 128MB 闸门使 `(2GiB,4GiB]` 在产品内不可达，属接口级潜伏缺陷；"实测"实为算术复算。

5. **C3-09 的修复方向不足**：只给 `parseRels` 加 500k 标签预算，仍能在预算内打出约 1.5 GB（属性放大）；应按 Relationship 条数/rels 字节数单独限流。

6. **C3-13 的"无条件补 `xl/`"措辞错**：实际是"剥 `/` 后若非 `xl/` 开头才补"，`/xl/...` 正常，坏的只是 `/custom/...` 这类包根绝对路径。

## 六、行号与归属勘误表

| 条目 | 原报告 | 核对结果 |
|---|---|---|
| C3-05 | `mainwindow.cpp:186` | 声明在 `mainwindow.h:186` |
| C3-05 | "相对目录文本链（如 dir/sub）" | 实为含根前缀的路径链（`E:/root/a/b`） |
| C3-04 | "分割/导出的同类 `chooseOutputPath`" | `chooseOutputPath` 仅分割使用，导出有独立的 `n<100000` 循环 |
| C3-09 | "3 处 `parseRels` 遗漏" | 2 个函数、3 个调用点 |
| C3-11 | 归因 `5569738` / D-32 | `uint` 上限来自 `f871a13`；`5569738` 引入 `static_cast<int>` |
| C3-13 | "无条件补 `xl/`" | 有条件（`!startsWith("xl/")`） |
| C3-14 | "ODT/ODP 同类问题均已修" | 与 C3-12 矛盾；实测未修 |

## 七、核实方式与边界

- 全部结论基于 `HEAD 28dbbe5` 源码阅读与跨文件 grep（"全仓无 clear"、"所有 `setContent` 的预算覆盖"等），关键项用真实插件代码路径的只读探针复现：C3-14/C3-12（docx/odt/odp 丢图）、C3-13（xlsx 丢表）、C3-09（rels 内存放大）、C3-10（blip 放大）、C3-02（Windows rename 语义）、C3-15（守卫布尔）。
- **未修改任何产品代码**：`git status` 与核对开始时一致；探针位于 `%TEMP%\fwprobe` 与 gitignore 的 `.workbuddy/tmp/` 下（`probe_rename/` 等）。
- 未复核原报告第五节的"已排除候选"（runner 退出码、QDom 实体展开等），也未重新跑 194 项测试基线；本次核对只回答"这 16 项是否真实存在"。

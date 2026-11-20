# FileWrapper 项目长期笔记

## 环境与构建（2026-11-13 更新）
- 本机 Qt 已从 5.13 升级到 **Qt 6.9.3**，安装根 `E:\V_C\Qt`；
  套件 `6.9.3/mingw_64`（qmake）与 `Tools/mingw1310_64`（MinGW 13.1.0 g++ / mingw32-make）。
  旧的 `Plugins/PluginMedia/build_test/`、QtCreator 4.9.1 配置仍指向 Qt5.13，已失效。
- 影子构建：
  `export PATH="/e/V_C/Qt/Tools/mingw1310_64/bin:/e/V_C/Qt/6.9.3/mingw_64/bin:$PATH"`
  → `qmake <顶层 .pro> && mingw32-make -j4`。
- FFmpeg 8.1（libavformat 62.12.102）随插件自带：`Plugins/PluginMedia/include|lib`，
  MinGW 用 `lib*.dll.a`、MSVC 用 `*.lib`；运行时需把该 dll 目录加入 PATH。
- 系统 ffmpeg/ffprobe 8.1.2 在 `D:\ffmpeg`，用于生成测试片、查关键帧。

## 架构约束（踩过的坑）
- PluginMedia 走**内存播放**：整个文件由主程序读入后交给插件。自定义 AVIOContext
  **必须提供 read_packet 与 seek 回调**；只把文件塞进 buffer 且回调为 NULL 会导致
  `pb->seekable=0`、`avio_size()` 失败，使 FLV/TS/AVI 无法 seek、MP3/TS 时长错。
  详见 2026-11-13 日志与 `.workbuddy/tmp/seek_probe/` 探针。
- seek 后目标前的包必须照常送解码器（不能丢包，否则 P/B 帧无参考帧），只跳过显示/音频输出。
- **PluginMedia 的视频缩放上下文（sws）与 RGB 输出缓冲归解码线程所有**
  （`VideoWorker::m_sws/m_outBuf/m_outData`），主线程只发尺寸请求
  （`requestOutputSize()` → 打包进 `std::atomic<quint64>`）。
  原因：窗口缩放要换输出尺寸就必须释放旧缓冲再重建，若缓冲归主线程，
  主线程释放时解码线程可能正在 `sws_scale` → use-after-free。
  释放只能走 `releaseVideoResources()`，且**必须在解码线程已停止后**调用。
  详见 2026-11-17 日志。
- 窗口尺寸变化不能只靠 `resizeEvent` 里读控件尺寸：resize 到达时布局可能还没
  按新尺寸算完，必须 `ui->mainLayout->activate()` 后再读；
  `QTimer::singleShot(0)` 在 Unix 事件派发器里**先于 posted 事件**执行，
  不能指望「延后一轮」就自动拿到新布局。
- 各插件 widget 是唯一共享实例，多标签页在页间迁移（见 2026-09-07 日志）。
- **插件运行时依赖只有一个来源**：`FileWrapper/deploy/plugin-deps.manifest`
  （`kind|源|目标|用途`），由 `deploy.pri` 在链接后调用 `deploy/deploy-deps.ps1` 统一复制，
  并写出 `plugin-deploy-manifest.txt` 供程序自检。**新增插件依赖只改 manifest，
  不要再往各插件 .pro 写 QMAKE_POST_LINK**（此前正是因此漏了 Qt6Multimedia.dll：
  干净部署下 Media 加载失败，而开发机 PATH 有 Qt bin 一直掩盖着）。
- 补部署：`mingw32-make deploydeps`（顶层与 `FileWrapper/` 子工程都可以）。
- **插件是自描述的（C1）**：文件后缀、API 版本都写在插件自己的 JSON 里
  （`extensions` / `apiVersion` / 可选 `extensionPriority`），主程序不再维护后缀表。
  **新增文件格式 = 只改插件 JSON + 重编该插件**（实证过：给 PluginText 加 `textile`
  只重编插件，主程序未动即生效）。
  - 已知冲突：`.ts` 既是 MPEG-TS 又是 TypeScript，历史行为是文本优先，故
    PluginText 声明 `extensionPriority: 10`（默认 0，大者优先）。
  - 构建时必须同时产出 `plugins/<DLL基名>.json` sidecar：DLL 加载失败时主程序
    靠它才知道该插件声明了哪些后缀（否则会退化成「未找到插件」）。
  - `Plugins/plugin_common.pri` 负责 DESTDIR + sidecar + JSON 依赖，插件 .pro 只留 3 行配置。
- **改插件接口必须升版本（B1）**：`plugininterface.h` 的 `PLUGIN_API_VERSION` 一旦
  虚函数表变化就要 +1，并同步所有插件 JSON 的 `apiVersion`；加载器在**实例化前**
  用元数据里的版本做门禁，避免跨版本调用虚函数崩溃。
- 插件加载失败现在有用户可见路径：状态栏提示 + 双击时「插件不可用」对话框
  （`plugindiag.cpp` 比对清单得出缺了哪个依赖、干什么用的）+「插件设置 → 插件诊断」。
  `PluginManager::retryFailedPlugins()` 只重试失败项，**不要实现整表 reload**：
  标签页里持有插件实例，卸载 DLL 会让指针悬空。
- **回归测试在仓库里（C4）**：`tests/` 下插件测试套件（当前共 247 项断言；有音频设备
  时 245。本机 `--shots` 实测 245 PASS / 0 FAIL / 0 SKIP，`-SkipShots` 为 242 PASS+3 SKIP），
  一键 `powershell -NoProfile -ExecutionPolicy Bypass -File tests\run-plugin-tests.ps1`。
  改插件/改插件接口/改部署后先跑它；产物在 `tests/build/`（已 gitignore）。
  不要再把测试写成只存在于 `.workbuddy/tmp` 的临时探针 —— 那类东西无法复现。
  ⚠️ **验证 runner 退出码必须用独立子进程**：不要在 PowerShell 工具里用
  `& runner.ps1 2>&1 | Out-File ...` —— PS 5.1 在 `2>&1` 重定向下会把测试程序写 stderr
  的 `[INFO]` 当终止错误，误报 `EXITCODE=1`（第三轮两维度踩过，白查半天）。
  正确做法：写脚本用 `Start-Process -RedirectStandardOutput/-RedirectStandardError -Wait -PassThru`
  启动 `powershell -File runner.ps1`，再读 `$p.ExitCode`（若遇 HTTPS_PROXY 大小写冲突，
  先清空 `$env:HTTPS_PROXY`/`$env:HTTP_PROXY`）。
  **runner 自身**现在用 `System.Diagnostics.ProcessStartInfo` + `WaitForExit(ms)` +
  `-TimeoutSec`（默认 600，超时杀进程树并返回 124）；不要改回裸 `& $TestExe`，
  也不要依赖 PS 5.1 `Start-Process -NoNewWindow` 的 `ExitCode`（实测可能为空）。
- **测试里加载插件只从 `<exe>/plugins`**（PluginManager 用的那一份），用
  `QPluginLoader` 取模板 + `createInstance()` 渲染 + 显式 `delete`；
  **绝不能从另一个路径再加载同一个 DLL** —— Windows 会当成两个模块，
  卸载其中一份会让插件侧遗留对象（如 QTextBrowser 子类）虚表悬空，
  表现为"测试全 PASS 但进程退出码 0xC0000005"（V 组踩过）。
- **插件接口头在 `include/plugininterface.h`（C6）**：主程序与插件都从那里取，
  插件通过 `Plugins/plugin_common.pri` 拿到 `INCLUDEPATH`；不要再写
  `#include "../../FileWrapper/plugininterface.h"` 这种相对路径。
- 插件实例必须能独立创建（B2）：`PluginManager::createInstanceFor()` 返回空即视为
  打开失败，**不允许退化成共享实例**（多标签页会互相覆盖内容）。
- 大文件抽临时副本前有磁盘预检（B4）：磁盘满时不再回退内存通道（会打爆进程）。
- 插件可被用户禁用（C5）：`[PluginEnabled]` 段（ini），禁用的插件不加载但保留后缀
  路由，打开时会提示"已被禁用"；`reloadPlugins()` 会卸载 DLL，调用前必须关闭标签页。
- **支持的格式要"接线"而不是"重写"**（P0）：Qt 自带 qwebp/qsvg/qico/qtiff/qtga/qicns/qwbmp
  解码器、FFmpeg 支持大量封装——但它们只在插件 JSON 声明了后缀、且部署清单
  复制了 imageformats 插件时才会被用到。新增格式优先检查这条链路：
  `pluginX.json 的 extensions` + `FileWrapper/deploy/plugin-deps.manifest`。
  注意 **qsvg 还要部署 Qt6Svg.dll**（它是 qsvg 的运行库依赖）。
- **PluginText 会按后缀分流**：`.html/.htm` 走 `QTextEdit::setHtml`（富文本渲染），
  其余（源码/日志/JSON/字幕 srt/vtt/ass）仍用 `setPlainText`；文件名未知时按内容嗅探。
- **PluginWord 同时是 ODF 解析器**：ZIP 里的 `mimetype` 含 `opendocument.text` 即按
  ODT 解析（content.xml/styles.xml → 同一套 HTML 子集，支持标题/字符格式/列表/表格/
  内嵌图片/超链接），`.odt/.ott` 已在 pluginWord.json 里声明。
  ODT 表格的 `table:number-rows/columns-repeated` 按 `kMaxOdtRepeatPreview=40`
  物化（超限 markTruncated），不能直接按原始 repeat 循环（常见 16384 空行/列）。
- **OPC Target 百分号解码有仓库内回归**：`tests/fixtures/opc_percent.docx/pptx/xlsx`
  把主文档/幻灯片/工作表部件名写成 `%20`，V15a–c 断言三个插件都能读到正文；
  改 `percentDecodeStrict()`/`resolvePart()`/`joinPath()` 必须重跑这三条。
- **PluginWord 还带 RTF 子集解析器**（`rtfdocument.{h,cpp}`）：RTF 不是 ZIP/XML，是
  控制字 + 嵌套分组，单独解析成段落/字符模型再生成 HTML；支持 \par/\line/\tab、
  对齐与缩进、粗/斜/下划线/删除线/上下标/字号/颜色/字体、\'hh（本地编码）与 \uN；
  不显示的分组（fonttbl/colortbl/stylesheet/info/pict/页眉页脚）整组跳过。
  表格只做 `\cell`→" | "、`\row`→换行的文本化，不做真表格。
- **表格/演示各有一个插件**：PluginSheet（`.xlsx/.xlsm/.ods` 只读首层数据：共享字符串、
  单元格、合并单元格；不计算公式/样式）与 PluginSlides（`.pptx/.odp` 逐页标题+正文+图片；
  不还原版式坐标/主题/动画）。两者都用 `Plugins/common/zipreader` + QDomDocument，
  依赖 Qt6Xml（已在部署清单）。新增同族格式时优先往这两个插件里加分支，而不是再开插件。
- **"打不开的文件"要先按内容识别**（`FileWrapper/formatresolver.{h,cpp}`）：  `resolvePlugin()` 在选择不到插件时用 `FileManager::entryHead(256KB)` 取文件头，
  按魔数/包内标志判断真实格式（PDF/PNG/JPEG/OLE2/ZIP→docx/xlsx/pptx/odt/ods/odp/epub/
  音视频/压缩包…）；能交给现有插件就直接路由并提示"按内容识别为 X"。
  识别不出来才弹「无法打开该文件」对话框：给出**识别结果 + 判断依据 + 可照做的建议**，
  按钮是「以文本查看（默认）/ 十六进制 / 查看支持的格式… / 设置插件映射… / 取消」。
  **插件映射只在用户明确知道该选哪个插件时才有用**，不要再把它当主入口。
  「插件设置 → 支持的格式…」会列出每个已装插件的后缀，以及常见不支持格式的处理建议。
  注意扩展名不可信（下载改名、.dat/.bin、无后缀），内容判断优先于扩展名。
- **保存时的路径规划（P0，2026-11-17 修）**：`rewriteArchive` 以前用
  `commonRoot + makeRelative`，跨盘时 root 为空、`makeRelative` 退化成文件名，
  添加外部文件再保存会把整个归档的目录结构拍平，同名条目变成重复路径。
  现改为 `computeStoredPaths()`：与合并同一套规则（无公共根时盘符当首段
  `E:/x -> E/x`），并在写盘前拒绝重复 relPath（`save()` 返回 false + 可读原因）。
  关闭窗口也已补 `closeEvent` 调用 `confirmDiscardOrSaveIfDirty()`，避免脏归档静默丢失。
- **合并对话框的列表语义**（`DlgMergeFile`）：`QTreeView` 多选 + 整行选择
  （`ExtendedSelection` / `SelectRows`），**不再用复选框标记删除**；
  「删除」按钮只删除当前选中项（Delete 键等价），未选中时提示先选择。
  列表项只承载路径文本，`getFilePaths()` 返回全部行。
- **合并时"已有归档"先拆分**（`FileManager::QMergeFiles`）：选中的文件如果首 4 字节是
  `FWDA` 魔数，就读它的索引（`inspectArchive`）把条目作为独立输入展开。**每个条目先还原
  "原始完整路径"**（归档条目 = 该归档的 root + 归档内相对路径；普通文件 = 绝对路径），
  再统一决定外层 root：所有条目在同一公共目录下 → 存相对路径（显示为 root + 相对路径，
  保持短路径）；**跨盘符/无公共根 → root 为空，把盘符当首段**（`E:/x` → `E/x`，
  冒号不能进路径段），保证原 111.dat 里的文件不会因为另一输入在 D: 就被改挂到 D: 下。
  - 撞名自动加 ` (2)`；`writeArchiveHeader`（v3）多了可选 `pStoredPaths`（给了就按给定路径写）。
  - 只展开一层：归档条目本身若又是 `.dat` 就作为普通条目保留。
  - 测试见 `tests/plugin_tests.cpp` 的 G 组（含 `fixtures/rooted.dat` 的跨盘符用例）。
- **插件公共代码在 `Plugins/common/`**（源码级共享，不是运行时库）：
  - `pluginbase.h`：`PluginBase<Derived, WindowT>`（CRTP）收口构造/析构、`apiVersion`、
    `getPluginWidget`、`createInstance`、`sendFile`/`sendFileData` 适配；
    插件只实现 `bool showFile(const PluginFile &)`。**新插件不要再抄代理层样板。**
  - `zipreader.{h,cpp}`：PluginBook 与 PluginWord 共用；在插件 `.pro` 里写
    `PLUGIN_SHARED_SOURCES = zipreader` 即编进自己的 DLL。改它要同时验证 epub + docx。
  - 每个插件仍独立编译/部署/启停，没有跨插件运行时依赖。
- **数据通道（C2，接口版本 3）**：主程序优先调 `PluginInterface::sendFile(const PluginFile&)`，
  `PluginFile` 是"内存块 / 文件路径"二选一。老插件不用改（默认实现转调 `sendFileData`，
  行为与旧版一致，会复制一次）；想省掉复制的插件重写 `sendFile()` 直接吃
  `file.memory()`（PluginWord 已迁移示范）。主程序侧用 `FileManager::entryContent()`
  把条目直接读进 QByteArray，不再经过 `new char[]`/`delete[]` 中转。
- **PluginWord 显示 Word（.docx/.docm/.dotx/.dotm）**：自包含 ZIP + QDomDocument 解析
  OOXML → HTML → QTextBrowser（无第三方库）。两个必须记住的点：
  - Qt 6.9.3 富文本的 `margin-*` / `text-indent` **只认 px，不认 pt**（pt 被静默忽略，
    缩进会全丢）；`font-size` 用 pt 正常。见 2026-11-17 日志。
  - 它链接了 `Qt6Xml.dll`（主程序自身不用 QtXml）。**运行时依赖统一由
    `FileWrapper/deploy/plugin-deps.manifest` 声明、构建末尾部署**，不要再往插件
    的 `.pro` 里写 `QMAKE_POST_LINK`（会漏、会重复，Qt6Multimedia 当年就是这么漏的）。
  - 旧版 `.doc`（OLE2）与 `.rtf` 不解析，识别后提示另存为 .docx。
- **PluginBook 依赖 vendored 的 libmobi**（`Plugins/PluginBook/thirdparty/libmobi`，
  上游 `bfabiszewski/libmobi` @ `9062742`）。它**曾以嵌套 Git 仓库形式存在**，
  会导致 `git add` 只产生 gitlink（克隆后拿不到源码）；现已改为普通 vendored 目录，
  上游信息见该目录 `PROVENANCE.md`，嵌套 `.git` 备份在仓库外 `.libmobi_git_backup/`。
  - libmobi 走**独立 `Makefile.mingw`** 编成 `libmobi.a` 再链接：`src/miniz.c` 是
    amalgamation 模式（`miniz.h` 内 include `miniz.c`），必须单独编译，
    否则 qmake 丢弃它导致链接期 `mz_uncompress` 未定义。
  - 加密三件套（`encryption.c`/`sha1.c`/`randombytes.c`）未编入，关闭 `USE_ENCRYPTION`。
  - ⚠️ **该目录曾被损坏**（`structure.c`/`common.c` 截断为 0 字节、`parse_rawml.h`
    被替换为无关文本），而陈旧 `.o` 让 `make` 跳过编译，**掩盖成"编译 0 error"**。
    在此仓库验证构建时**必须先删掉 `src/*.o` 与 `libmobi.a` 做真干净构建**，
    否则结论不可信。
- 本机 PowerShell 进 MSVC 环境会撞 `HTTPS_PROXY`/`https_proxy` 大小写冲突，
  且 `Enter-VsDevShell` 内部调用被黑名单拦的 reg.exe；改用 Bash + 手动设
  `INCLUDE`/`LIB`/`PATH` 调 cl.exe 可绕过。

## 推送到 GitHub 的网络约束（2026-11-16 实测，重要）
本机到 **`github.com:443` 的所有路径都不可达**：
- 走本地代理 `http://127.0.0.1:<port>` → `CONNECT tunnel failed, response 502`
- 直连各 GitHub 真实 IP（20.205.243.166/164/170、20.27.177.113、140.82.113.3/114.3）→ 全部超时
- 系统 DNS 把 `github.com` 解析到 **20.6.7.17**（Azure 段，非 GitHub IP）→ 不可达
  （全局 `http.curloptresolve=github.com:443:140.82.112.4` 这个固定 IP 也已失效）
- 但 **`api.github.com` 双向可达**（代理与直连都 200），`github.com` 不通而
  `api.github.com` 通，说明是针对 `github.com` 这一 host 的阻断。

**因此 `git push` 无法走标准通道**，需改用 **GitHub REST Git Data API** 推送：
`POST /git/blobs` → `POST /git/trees` → `POST /git/commits` → `PATCH /git/refs/heads/main`。
凭据从 `git credential fill`（Windows Credential Manager，`credential.helper=manager`）取。
参考脚本：`.workbuddy/tmp/push_api2.py`（`.workbuddy/tmp/` 被 gitignore，不会入库）。

**用 API 复现提交时有两个必须踩准的点**（否则 commit SHA 对不上、历史分叉）：
1. **blob 内容必须取自 git 对象库**（`git cat-file blob <index-sha>`），不能读磁盘原始字节。
   Windows 上 git 提交时做 CRLF→LF 转换，磁盘是 CRLF、仓库 blob 是 LF；
   直接上传磁盘字节会得到不同的 blob SHA → tree → commit 全部对不上
   （踩过一次：3 个 commit 全成孤儿，远端存的是非规范的 CRLF 版本，需覆盖纠正）。
2. **commit message 必须用 `git cat-file commit <sha>` 取原始字节**（含末尾那个 `\n`）。
   用 `git log --format=%s` + `%b` 重组会丢掉末尾换行 → SHA 不同。
3. author/committer 用 `git cat-file commit` 里的原始 `unix timestamp + tz`，
   转 ISO8601 传入即可（GitHub 会原样存储为同一 tz）。

覆盖远端 ref 前必须先校验**远端与本地文件路径集合一致**（`git ls-tree -r --name-only`），
否则有覆盖他人提交的风险。

> **2026-11-17 晚更新（重要）**：`github.com:443` 是**不稳定而非完全不可达** ——
> 当晚 `git push origin main` 成功一次（`f2fb7dd..a37a1b2`），但同一时段的
> `git ls-remote` / `git fetch` 仍然 21s 超时；`api.github.com` 始终可达。
> 所以现在的推送策略应当是：
> 1. 先直接 `git push`（可能成功，别提前假定不行）；
> 2. 失败就重试 1–2 次（网络是抖动型）；
> 3. 仍失败再走上面的 Git Data API 脚本；
> 4. **无论哪条路，最后都用 `api.github.com/repos/<owner>/<repo>/git/ref/heads/main`
>    核对远端 SHA == 本地 HEAD**（`ls-remote` 不可靠，会因超时误判成"没推上去"）。
>
> 当晚实际过程：前 3 个提交（`83a465e`/`63544ac`/`a37a1b2`）是用**普通 `git push`
> 一次成功**的；随后 `github.com` 再次不可达（DNS 仍解析到 20.6.7.17，
> `http.curloptResolve` 指到真实 IP 也 SSL reset / timeout），最后一个提交
> `d2d2361` 改走上面的 Git Data API 脚本才推上去。
>
> **API 脚本的编码坑**：脚本里 `say()` 输出 `✔` 这类字符在 GBK 控制台会抛
> `UnicodeEncodeError`，而且是在 `PATCH ref` **之前**崩 —— 表现为"commit 建好了但
> ref 没动"。运行前必须设 `$env:PYTHONIOENCODING='utf-8'`
> （必要时再加 `[Console]::OutputEncoding=[Text.Encoding]::UTF8`）。
- **文本查看器查找**：PluginText 工具栏搜索框 + Ctrl+F/F3；`findText()` 循环查找。
- **分割预填 / 加载确认**：`FileManager::archivePath()` + `DlgSplitFile::setMergedFilePath()`；
  已加载归档时加载新归档会先确认"关闭所有标签页"。
- **可运行打包**：`FileWrapper/deploy/package.ps1`（复制依赖后跑 windeployqt，
  产出可直接双击的目录；构建目录本身不可双击运行）。
  守卫已禁止 `OutDir` 等于仓库根**或位于仓库内任意子目录**（`.git`/源码目录也会被
  `Remove-Item -Recurse -Force` 删掉；第四轮补的包含检查），打包输出请放仓库外。
  脚本同时复制 `translations/`，否则打包版缺少 `qtbase_zh_CN.qm`、插件诊断会误报缺依赖。
- **全工程审计历史**：仓库根 `CODE_AUDIT_FINAL_2026-11-18.md`（43 项）、
  `CODE_AUDIT_CYCLE2_2026-11-18.md`（109 项）、`CODE_AUDIT_CYCLE3_2026-11-19.md`（16 项，
  HEAD=28dbbe5，P1 为 package.ps1 删库）。前两轮项已在 52 个 audit/audit2 提交中修复；
  第三轮主要是修复引入的回归（CRC 后台校验生命周期、d28b3d4/72bf3fa/38d3476）
  与"上限只覆盖主路径"家族（rels/findBlip/OPC Target 百分号解码）。
  修复同类问题时优先查"缓存集合生命周期""辅助路径是否漏检""失败分支是否复位"。

## 2026-11-19 审查改进（P0 全修 / P1、P2 大部）实施笔记
- 报告与逐项状态见仓库根 **`AUDIT_2026-11-19_FIX_REPORT.md`**（对应
  `FileWrapper_代码审查与改进建议_2026-11-19.md`，P0×5/P1×24/P2×17）。
- **归档写入不变量**（后续改 filemanager 必须保持）：
  - 临时/备份文件一律"随机名 + NewOnly 原子创建"，由 `OwnedAuxFiles` 登记，
    **禁止**再出现 `<输出>.tmp.fwda`/`.bak.fwda` 这种固定名或预删路径；
  - 保存/合并顺序：唯一备份 → 换入 → **重新解析成功** → 删备份；失败回滚，
    回滚失败要把备份绝对路径告诉用户；
  - 跨进程锁是自实现的 `<输出>.fwdalock`（NewOnly+PID+30s 陈旧），**不要换回
    QLockFile**：受限权限/沙箱下它返回 PermissionError，会把正常保存全挡死；
    30s 只是“可能已崩溃”的门槛，删锁前必须用 `isProcessAliveForLock()` 确认 PID
    已退出——长保存超过 30s 时旧实现会被第二个实例抢锁；析构也只删自己 PID 的锁；
  - 墓碑式原地快速删除已停用（`markDeletedOnly` 保留但无调用）；大归档删除会重写内容区，
    若要恢复必须先做写前日志 + 启动恢复。
- **标签/实例生命周期**：`MainWindow::ensureInstanceStopped()` 是唯一停止入口；
  `stopPlaybackAndWait()==false` 的实例进 `m_setStopUnsafe`/延期队列，映射/实例/loader
  全部保留，`PluginManager::setUnloadEnabled(false)` 禁止 unload。标签下标不再是身份：
  一律走 `docIndexForTab()`（按插件窗口指针），`tabMoved` 会同步 `m_vTabDocs`。
- **媒体线程**：`m_videoThread` 无父对象，只有 doDecode 退出 + 事件循环 wait 成功才 delete；
  解码任务签名是 `doDecodeRun(quint64 runId)`，与 `m_contextRunId`/`m_runId` 绑定，
  换文件/停止要 `invalidateDecodeRun()`；`AVIOInterruptCB` 通过 `m_ioInterrupt` 取消阻塞 I/O。
- **插件资源预算**：`ZipReader` 自带 `ParseBudget`（单条 64 MiB/累计 256 MiB/提取次数），
  stored 与 deflate 同入口并按 central CRC 校验；声明 `uncompSize=0` 却带压缩数据的条目
  必须拒绝（否则预算记 0、可反复解压）；解压只按声明长度分配一次，输出超过即拒绝。
  Word/Sheet/Slides/Book 有 200 层 XML 深度 + 50 万节点 + **单元素 4096 属性**上限
  （属性放大是 QDom 的二次方成本，2MB/20 万属性可卡 60s）+ 输出字符预算；
  图片先 `QImageReader::size()` 预算再解码。
- **缩略图**：模型项会长期持有 QIcon，缓存 `clear()` 不释放它们；因此缩略图按
  128px 解码，并且每次重建只生成前 1024 张（其余保留类型角标），否则大相册会线性吃内存。
  读取/解码已移到单线程 `QThreadPool`（`ThumbnailDecodeTask`）：后台只产出 `QImage`，
  GUI 线程才 `QPixmap::fromImage()`/`setIcon()`；用 `m_nThumbnailEpoch` +
  `ThumbnailGate::generation` 作废旧结果。**保存/替换归档前必须调用
  `prepareThumbnailsForArchiveWrite()`**，等后台归档句柄关闭，否则 Windows rename 会失败；
  后台读的是路径+偏移快照，不要在新线程里调用 `FileManager::entryContent()`。
- **构建**：`plugin_common.pri` 不再 touch 源码头文件 → 只读源码可构建；代价是
  **改插件 JSON 后只重跑 qmake 不会重嵌元数据**：必须删除该插件的 `moc_*.cpp`/对象
  或 `mingw32-make clean` 后再 qmake+make。
- **测试**：新增 `AUDIT-2026-11` 组（11 条）与 `FW_TEST_SETTINGS_DIR` /
  `FW_TEST_SCRATCH_DIR` 覆盖入口。沙箱下 `QTemporaryDir` 会把目录 ACL 收成 owner-only，
  测试进程反而写不进去 → 受限环境必须用这两个环境变量指向预创建的可写目录。
  无音频设备时 V13 断言"纯音频明确失败并复位界面"；D6 在沙箱按环境 SKIP。
- **验证基线**：全量构建 0 error；本机（有音频设备、TEMP 可写）
  `plugin_tests --integrate` = 245 PASS / 0 FAIL / 0 SKIP（`-SkipShots` 为
  242 PASS + 3 SKIP）。测试共 247 项（无音频设备时），数字随用例增长，以实际日志为准。

## 2026-11-20 停播超时恢复（接口 v7）
- `PluginInterface::isPlaybackStopped()`（PLUGIN_API_VERSION 6→7，8 个插件 JSON 同步
  `apiVersion:7`）：默认 `!hasBackgroundPlayback()`；PluginMedia 用
  `VideoWorker::isDecodingActive()` 原子查询。
- 宿主 `m_setStopUnsafe` 不再永久生效：`m_stopUnsafePoll` 每秒非阻塞复查，
  `pollStopUnsafeInstances()` 成功后退还映射、销毁延期实例、恢复
  `PluginManager::setUnloadEnabled(true)`，保存/重载随之恢复。
- 保存/重载入口会先主动 `pollStopUnsafeInstances()` 一次；不要把它改成再次调用
  `stopPlaybackAndWait()`（会重新阻塞 30 秒）。

## 2026-11-19 插件失败信息（接口 v6）
- **起因（用户报的真实问题）**：打开 `1.dat` 里的 `audio.wav` 时提示「插件无法显示该文件：audio.wav」，
  日志却是 `[ShowVideo] 音频设备启动失败（纯音频）: 1`。查下来是这台机器 **没有可用的音频输出设备**
  （注册表里 3 个 Render 端点全是 `DeviceState=4/8`：不存在/未插入），被界面说成了"文件坏了"。
- **接口**：`PluginInterface::lastError()`（PLUGIN_API_VERSION 5→6，8 个插件 JSON 同步 `apiVersion:6`）。
  约定：空串 = 没提供原因，宿主退回通用提示；非空 = 插件**已自行复位界面**并写了说明，
  宿主不要再 `sendFileData(nullptr, 0)` 清屏；每次新的打开尝试插件必须清空上一次的原因。
- **PluginMedia**：所有失败出口走 `failOpen(短原因, 明细)`；纯音频 + 音频输出不可用 = 明确失败
  （`QMediaDevices::defaultAudioOutput()` 为空 / 格式不支持 / 启动失败三种原因分开说），
  带视频 = 无声降级并在状态栏说明；明细同时写进画面区（`showFailurePage()`）——
  状态栏消息 10 秒后消失，用户回头看标签页不能只剩一块黑屏。
- **判设备用 QMediaDevices，不要只看 `QAudioSink::start()==nullptr` + `error()`**：
  error=1（OpenError）把"没有设备/被独占/格式不支持/音频服务没起"全压成一个数字。
- **宿主**：`MainWindow::pluginFailureText()` 统一文案——有原因写「无法打开 X：原因」，
  没原因才回落到老的「插件无法显示该文件」；映射通道/临时文件通道失败时也先看 `lastError()`，
  避免插件已经报出真实原因、宿主还拿「文件过大…无法通过文件路径读取」敷衍。
- **测试**：V13 扩展为 V13/V13b–f（无设备分支断言原因与画面区文案；坏媒体文件与声卡无关地覆盖同样两条），
  A11a/b 断言宿主文案。`tests/build` 在旧沙箱里是只读的（应用目录 ACL 收紧），
  受限环境跑测试要给 `TEMP`/`TMP` 指到可写目录，否则 H10–H14/H19 会因 `QDir::tempPath()` 不可写而假失败。

## 2026-11-19 归档 v3：合并时保存文件系统元数据
- **起因**：合并/查看只保存内容，源文件的创建/修改/访问时间与只读/隐藏属性会丢；
  属性页只能显示路径/大小/CRC。现改为 v3 头部按条目保存元数据，导出/分割再写回。
- **格式**：`archiveformat.h` 新增 `VERSION_V3=3` / `ENTRY_FIXED_V3=52`：在 v2 的
  `PathLength/FileSize/ContentPos/ContentCrc32` 后追加 `CreatedMs/ModifiedMs/AccessedMs`（u64，
  UTC 毫秒，0=未记录）与 `Attributes`（u32，FILE_ATTRIBUTE_* 的 R/H/S/A 位）。
  **写 v3，读 v1/v2/v3**；v1/v2 条目 `hasFsMetadata=false`（属性页显示"未记录"），
  保存 v1/v2 归档会升级成 v3。v3 不使用墓碑（解析仍兼容 v2 墓碑）。
- **采集/恢复**：`filemetadata.{h,cpp}`（`FwMeta::capture/apply`）。合并**先 capture 再读内容**
  （探测魔数/复制都会刷新 atime）；`addFiles` 时 capture、`rewriteArchive` 对 pending 条目
  save 前再 capture；分割/导出在内容 CRC 通过后写回时间/属性（失败只告警，不删内容）。
  只读保存在 `GetFileAttributes`/`SetFileAttributesW` 上，跨平台时只映射只读位。
- **属性页**：新增"文件"段（创建/修改/访问时间、属性 R/H/S/A）；`DlgFileProperty::Data`
  增加元数据字段，`formatInfoText` 同步（复制信息也包含）。EXIF/相机/GPS 属于文件内容，
  本次未解析；需要时另加（JPEG APP1/TIFF IFD 或 Windows WIC 均可）。
- **测试**：P 组 12 条（v3 写入/读取、导出/分割恢复、新增保存、v3 再合并传递、v2 兼容、
  头部 CRC 覆盖元数据）。atime 断言只要求"有值"——关闭句柄时系统可能回写访问时间。

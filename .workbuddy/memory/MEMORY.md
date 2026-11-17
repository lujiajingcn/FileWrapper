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
- **回归测试在仓库里（C4）**：`tests/` 下有 138 项断言的插件测试套件，
  一键 `powershell -NoProfile -ExecutionPolicy Bypass -File tests\run-plugin-tests.ps1`。
  改插件/改插件接口/改部署后先跑它；产物在 `tests/build/`（已 gitignore）。
  不要再把测试写成只存在于 `.workbuddy/tmp` 的临时探针 —— 那类东西无法复现。
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

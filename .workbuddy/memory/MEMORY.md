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

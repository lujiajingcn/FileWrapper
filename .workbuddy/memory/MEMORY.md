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

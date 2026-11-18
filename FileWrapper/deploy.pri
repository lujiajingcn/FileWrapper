# ============================================================================
# deploy.pri —— 插件运行时依赖部署的唯一入口（2026-11-17 引入）
#
# 背景：本工程每个插件都可能带「主程序自己不链」的运行库（Qt6Xml / Qt6Pdf /
# Qt6Network / Qt6Multimedia / ffmpeg …）。以前这些复制动作分散在三个 .pro 里，
# 而且 Qt6Multimedia 根本没人复制；开发机 PATH 里有 Qt bin 所以看不出来，换台机器
# 就变成「插件加载失败且程序毫无提示」。
#
# 现在：依赖清单一律写在 deploy/plugin-deps.manifest（唯一来源），主程序链接完成后
# 由 deploy/deploy-deps.ps1 统一复制到程序目录，并写出 plugin-deploy-manifest.txt
# 供程序运行时自检（见 FileWrapper/plugindiag.cpp）。
#
# 新增插件依赖：只改 manifest，不要再到各插件 .pro 里写 QMAKE_POST_LINK。
# ============================================================================

win32 {
    CONFIG(debug, debug|release) {
        DEPLOY_APP_DIR = $$OUT_PWD/debug
    } else {
        DEPLOY_APP_DIR = $$OUT_PWD/release
    }

    # FFmpeg 发行包在仓库外（与仓库同级），允许不存在：脚本只告警，
    # 运行时自检会把缺失项列给用户，而不是让构建莫名其妙地失败。
    DEPLOY_FFMPEG_BIN = $$PWD/../../ffmpeg-n8.1.2-34-g9b6c8969e0-win64-gpl-shared-8.1/ffmpeg-n8.1.2-34-g9b6c8969e0-win64-gpl-shared-8.1/bin

    # 路径必须经 $$shell_path() 归一化成 Windows 反斜杠形式（cmd 的 copy 不认正斜杠），
    # 并用 \" 包住，避免安装路径含空格时被拆成多个参数。
    DEPLOY_SCRIPT   = $$shell_path($$PWD/deploy/deploy-deps.ps1)
    DEPLOY_MANIFEST = $$shell_path($$PWD/deploy/plugin-deps.manifest)

    DEPLOY_PS_ARGS = \
        -Manifest \"$$DEPLOY_MANIFEST\" \
        -TargetDir \"$$shell_path($$DEPLOY_APP_DIR)\" \
        -QtBin \"$$shell_path($$[QT_INSTALL_BINS])\" \
        -QtPlugins \"$$shell_path($$[QT_INSTALL_PLUGINS])\" \
        -FfmpegBin \"$$shell_path($$DEPLOY_FFMPEG_BIN)\" \
        -RepoRoot \"$$shell_path($$PWD/..)\"

    DEPLOY_CMD = powershell -NoProfile -ExecutionPolicy Bypass -File \"$$DEPLOY_SCRIPT\" $$DEPLOY_PS_ARGS

    # 主程序链接完成后自动部署（与原 ffmpeg 复制的时机一致）。
    # manifest/脚本也是构建输入：只改清单时 make 必须重链接并重新部署，
    # 否则运行时清单会停留在旧版本。
    PRE_TARGETDEPS += $$DEPLOY_MANIFEST $$DEPLOY_SCRIPT
    QMAKE_POST_LINK = $$DEPLOY_CMD

    # 也可以单独执行：mingw32-make deploydeps
    deploydeps.target   = deploydeps
    deploydeps.commands = $$DEPLOY_CMD
    deploydeps.depends  += $$DEPLOY_MANIFEST $$DEPLOY_SCRIPT
    QMAKE_EXTRA_TARGETS += deploydeps
}

# ============================================================================
# plugin_common.pri —— 插件工程公共设置（C1/B1 引入）
#
# 用法：在每个插件的 .pro 里（TARGET / PLUGIN_METADATA_JSON / PLUGIN_CLASS_HEADER
# 都设置之后）：
#     PLUGIN_METADATA_JSON = $$PWD/pluginXxx.json
#     PLUGIN_CLASS_HEADER  = $$PWD/pluginxxx.h
#     include(../plugin_common.pri)
#
# 做三件事：
#   1) 输出目录统一到主程序 exe 同级的 plugins/
#      （以前这段在 6 个 .pro 里各抄一遍，改一处要改六处）
#   2) 把插件元数据 JSON 复制成 sidecar：plugins/<DLL 基名>.json
#      **为什么需要 sidecar**：主程序要在「插件 DLL 加载失败」时仍然知道
#      这个插件声明了哪些文件后缀（C1 的后缀表已经搬到插件 JSON 里），
#      否则用户打开 .docx 只会看到「未找到插件」，而不是「PluginWord 不可用，
#      缺 Qt6Xml.dll」这种能直接照做的提示。DLL 加载失败时读不到内嵌元数据，
#      只能读同目录的 sidecar。
#   3) 让「只改元数据 JSON」也能触发重编
#
# 插件 JSON 会以两种形式存在（内容同一个文件）：
#   - 内嵌在 DLL 里（Q_PLUGIN_METADATA），主程序校验插件时用这份（权威）；
#   - 复制成 sidecar，加载失败时用这份（能读到；只用于"后缀归谁"的路由）。
# ============================================================================

# ---- 插件接口头：include/ 是插件与主程序的唯一契约，插件不再依赖主程序源码树 ----
INCLUDEPATH += $$PWD/../include

# ---- 输出目录：主程序 exe 同级 plugins/（自动适配 debug/release 与构建根目录）----
CONFIG(debug, debug|release): PLUGIN_OUT_DIR = $$OUT_PWD/../../FileWrapper/debug/plugins
else:                        PLUGIN_OUT_DIR = $$OUT_PWD/../../FileWrapper/release/plugins
DESTDIR = $$PLUGIN_OUT_DIR

# ---- sidecar 元数据 ----
# 注意：本 .pri 里 $$PWD 指向的是 **本文件** 所在目录（Plugins/），不是包含它的
# 插件目录，所以调用方必须把 PLUGIN_METADATA_JSON 设成完整路径
# （`PLUGIN_METADATA_JSON = $$PWD/pluginXxx.json`），这里直接用，不再拼 $$PWD。
!isEmpty(PLUGIN_METADATA_JSON) {
    PLUGIN_SIDECAR_DST = $$shell_path($$PLUGIN_OUT_DIR/$${TARGET}.json)
    PLUGIN_SIDECAR_SRC = $$shell_path($$PLUGIN_METADATA_JSON)
    QMAKE_POST_LINK = $$QMAKE_COPY \"$$PLUGIN_SIDECAR_SRC\" \"$$PLUGIN_SIDECAR_DST\" || echo sidecar-copy-skipped

    # ---- 让元数据 JSON 成为真正的构建输入 ----
    # Q_PLUGIN_METADATA(IID ... FILE ...) 的内容是 **moc 在生成 moc_xxx.cpp 时读取并内嵌** 的，
    # 但 qmake 只把 .h 当 moc 的依赖，不会把 JSON 加进去 —— 结果就是「只改 JSON」时
    # make 认为无事可做，DLL 里仍然是旧元数据（sidecar 也不会刷新），排查起来非常费解。
    # 这里加一个戳文件：JSON 比戳新时，先把插件类头文件的 mtime 更新（触发 moc 重跑），
    # 再更新戳文件。PRE_TARGETDEPS 保证它在链接目标之前先执行。
    !isEmpty(PLUGIN_CLASS_HEADER) {
        PLUGIN_META_STAMP = $$OUT_PWD/$${TARGET}.meta.stamp
        PLUGIN_HDR = $$shell_path($$PLUGIN_CLASS_HEADER)
        PLUGIN_STAMP = $$shell_path($$PLUGIN_META_STAMP)

        # 更新头文件 mtime 用 cmd 的 "copy /b 文件+,, 文件"（就是"触碰"，不改内容），
        # 再写戳文件。不要在这条 recipe 里用 PowerShell 管道：recipes 走 cmd，
        # 反引号/`^|` 转义在双引号参数里会被原样传给 PowerShell 而报错。
        meta_stamp.target   = $$PLUGIN_META_STAMP
        meta_stamp.depends  = $$PLUGIN_METADATA_JSON
        meta_stamp.commands = $$QMAKE_COPY /b \"$$PLUGIN_HDR\"+,, \"$$PLUGIN_HDR\" >nul 2>&1 || echo touch-skip$$escape_expand(\\n\\t) echo.> \"$$PLUGIN_STAMP\"
        QMAKE_EXTRA_TARGETS += meta_stamp
        PRE_TARGETDEPS      += $$PLUGIN_META_STAMP
    }
}

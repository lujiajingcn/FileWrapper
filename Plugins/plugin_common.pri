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

# ---- 共享头/源码目录（Plugins/common/）----
# 所有插件都能 include 到 common/pluginbase.h（代理层基类）；
# 需要额外共享源码时，在插件 .pro 里写
#     PLUGIN_SHARED_SOURCES = zipreader
# 会把这些 common/<name>.cpp/.h 编进**本插件自己**的 DLL。
#
# 为什么是"源码共享"而不是静态库/动态库：
#   * 插件要能独立编译、独立部署、独立禁用（C5），不引入跨插件运行时依赖；
#   * 共享的只有极少数无状态工具（pluginbase.h / ZipReader），每个 DLL 多编一份
#     的代价可以忽略，换来构建简单、无库顺序/配置矩阵问题。
# 注意：改 common/ 下的文件会影响所有引用它的插件，验证要覆盖每一种格式。
INCLUDEPATH += $$PWD/common
!isEmpty(PLUGIN_SHARED_SOURCES) {
    for(shared_src, PLUGIN_SHARED_SOURCES) {
        SOURCES += $$PWD/common/$${shared_src}.cpp
        HEADERS += $$PWD/common/$${shared_src}.h
    }
}

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
    QMAKE_POST_LINK = $$QMAKE_COPY \"$$PLUGIN_SIDECAR_SRC\" \"$$PLUGIN_SIDECAR_DST\"
}

# ---- 内嵌元数据的构建约定（P2-17：只写构建目录，不修改源码树）----
#
# 以前为了「只改 JSON 也触发重编」会去 touch 源码头文件（copy /b hdr+,, hdr）：
#   * 源码树必须可写 —— 只读源码构建、受限权限环境直接构建失败；
#   * 依赖 Windows cmd 的小技巧，跨平台无意义；
#   * 构建过程改源码时间戳，干扰增量构建与版本管理。
#
# 现在构建过程完全不碰源码树：JSON 只作为构建输入登记（DISTFILES）。
# ⚠️ DISTFILES 不是 moc 的构建依赖：只改 JSON 后跑 qmake + make，moc_*.cpp
# 不会重新生成，内嵌元数据仍是旧值（sidecar 也只有重新链接时才复制）。
# 修改插件 JSON 后必须让该插件的 moc/对象失效：删除构建目录里的
# moc_<plugin>.cpp 与对应 object，或直接 `mingw32-make clean` 后重新 qmake+make。
# QtCreator 里「清理 → 执行 qmake → 重新构建」等价。内嵌元数据的权威性不变。
!isEmpty(PLUGIN_METADATA_JSON) {
    DISTFILES += $$PLUGIN_METADATA_JSON
}

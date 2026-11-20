#-------------------------------------------------
#
# Project created by QtCreator 2023-06-30T09:13:04
#
#-------------------------------------------------

QT       += core gui

# 插件接口头在仓库根 include/：主程序与插件都从那里取，接口单一来源
INCLUDEPATH += $$_PRO_FILE_PWD_/../include

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = FileWrapper
TEMPLATE = app

# The following define makes your compiler emit warnings if you use
# any feature of Qt which has been marked as deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if you use deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

CONFIG += c++17

# 核心源文件清单与 tests/plugin_tests.pro 共用（见 core.pri），main.cpp 只属于主程序
include(core.pri)
SOURCES += main.cpp

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

# ============================================================================
# 插件运行时依赖部署（2026-11-17 统一）
#
# 以前这里单独复制 ffmpeg 的 5 个 DLL，PluginPdf.pro 复制 Qt6Pdf/Qt6Network，
# PluginWord.pro 复制 Qt6Xml —— 三处零散、责任不清，而且 Qt6Multimedia.dll
# （PluginMedia 的音频输出依赖）根本没人复制：开发机 PATH 里有 Qt bin 所以看不出来，
# 换台机器就「插件加载失败」，程序还不给任何提示。
#
# 现在：依赖清单一律写在 deploy/plugin-deps.manifest（唯一来源），链接完成后由
# deploy/deploy-deps.ps1 统一复制到 exe 目录，并生成 plugin-deploy-manifest.txt
# 供程序自检（见 plugindiag.cpp）。新增插件依赖只改 manifest，不要再写 POST_LINK。
#
# 编码约定（P2-17）：本工程 qmake 文件统一使用 UTF-8（无 BOM）。
# 历史上有部分 .pro 注释曾被按 GBK 重存成乱码，已按当前约定恢复；修改时请保持 UTF-8。
# ============================================================================
RESOURCES += $$_PRO_FILE_PWD_/../resources/resources.qrc
win32: RC_FILE += $$_PRO_FILE_PWD_/../resources/appicon.rc

include(deploy.pri)

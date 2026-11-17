#-------------------------------------------------
#
# Project created by QtCreator 2023-06-30T09:13:04
#
#-------------------------------------------------

QT       += core gui

# 插件接口头放在仓库根 include/：主程序与插件都从那里取，接口单一来源
INCLUDEPATH += $$PWD/../include

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

SOURCES += \
        builtinviewer.cpp \
        dlgabout.cpp \
        dlgmergefile.cpp \
        dlgoutputfile.cpp \
        dlgpluginmap.cpp \
        dlgsplitfile.cpp \
        filemanager.cpp \
        main.cpp \
        mainwindow.cpp \
        pluginmanager.cpp \
        plugindiag.cpp

HEADERS += \
        archiveformat.h \
        builtinviewer.h \
        dlgabout.h \
        dlgmergefile.h \
        dlgoutputfile.h \
        dlgpluginmap.h \
        dlgsplitfile.h \
        filemanager.h \
        mainwindow.h \
        $$PWD/../include/plugininterface.h \
        pluginmanager.h \
        plugindiag.h

FORMS += \
        dlgabout.ui \
        dlgmergefile.ui \
        dlgoutputfile.ui \
        dlgsplitfile.ui \
        mainwindow.ui

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
# ============================================================================
include(deploy.pri)

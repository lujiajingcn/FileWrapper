#-------------------------------------------------
# Project created by QtCreator 2023-07-09T09:39:23
#-------------------------------------------------

# Qt6 迁移：PDF 渲染改用 Qt 自带的 QtPdf 模块（QPdfDocument）。
# 原实现链接 Qt5 编译的 libQt5Pdfium.a / Qt5Pdfium.dll，后者导入 Qt5Core.dll
# 与 Qt5Gui.dll；本机升级到 Qt6 后这两个 DLL 不存在，QPluginLoader 加载
# PluginPdf.dll 时解析依赖链失败，报「找不到指定的模块」(ERROR_MOD_NOT_FOUND 126)。
QT       += core gui widgets pdf

TARGET = PluginPdf
TEMPLATE = lib

DEFINES += QT_DEPRECATED_WARNINGS

# Qt6 头文件要求 C++17
CONFIG += c++17

SOURCES += \
        main.cpp \
        mainwindow.cpp \
        pluginpdf.cpp

HEADERS += \
        mainwindow.h \
        pluginpdf.h

FORMS += \
        mainwindow.ui

# 插件始终输出到主程序 exe 同级的 plugins/ 目录（自动适配 debug/release 与构建根目录）
# 公共设置（输出目录 + 插件元数据 sidecar：plugins/<DLL基名>.json）
PLUGIN_METADATA_JSON = $$PWD/pluginPdf.json
PLUGIN_CLASS_HEADER  = $$PWD/pluginpdf.h
include(../plugin_common.pri)

# 运行依赖（Qt6Pdf.dll 及其 run_depends Qt6Network.dll）已统一到
# FileWrapper/deploy/plugin-deps.manifest，由主程序构建末尾的 deploy.pri 负责部署，
# 不再在这里写 QMAKE_POST_LINK（否则「谁负责部署」会散落到每个插件里）。
# 单独构建本插件时不会部署依赖，请构建 FileWrapper.pro（或 mingw32-make deploydeps）。

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

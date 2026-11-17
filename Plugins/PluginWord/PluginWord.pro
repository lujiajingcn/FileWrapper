#-------------------------------------------------
# PluginWord - Word 文档预览插件（OOXML：.docx/.docm/.dotx/.dotm）
#
# 只用 Qt 自带模块 + zlib，无第三方库：
#   * zipreader.cpp  自包含 ZIP 读取器（链接 MinGW 自带静态 libz.a）
#   * worddocument.cpp  OOXML -> HTML（QDomDocument，需要 Qt6Xml.dll）
#-------------------------------------------------

QT       += core gui widgets xml

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = PluginWord
TEMPLATE = lib

DEFINES += QT_DEPRECATED_WARNINGS

CONFIG += c++17

SOURCES += \
        mainwindow.cpp \
        pluginword.cpp \
        worddocument.cpp \
        zipreader.cpp

HEADERS += \
        mainwindow.h \
        pluginword.h \
        worddocument.h \
        zipreader.h

FORMS += \
        mainwindow.ui

# 自包含的 ZIP 读取器（zipreader.cpp）用 zlib 解压 deflate 条目。
# 链接 MinGW 自带的静态 libz.a（无需额外部署 zlib1.dll）。
LIBS += -lz

# 插件始终输出到主程序 exe 同级的 plugins/ 目录（自动适配 debug/release 与构建根目录）
# 公共设置（输出目录 + 插件元数据 sidecar：plugins/<DLL基名>.json）
PLUGIN_METADATA_JSON = $$PWD/pluginWord.json
PLUGIN_CLASS_HEADER  = $$PWD/pluginword.h
include(../plugin_common.pri)

# 运行依赖（Qt6Xml.dll：主程序本身不用 QtXml，windeployqt 不会自动带上）已统一到
# FileWrapper/deploy/plugin-deps.manifest，由主程序构建末尾的 deploy.pri 负责部署。
# 单独构建本插件时不会部署依赖，请构建 FileWrapper.pro（或 mingw32-make deploydeps）。

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

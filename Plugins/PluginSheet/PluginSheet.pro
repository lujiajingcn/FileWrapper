#-------------------------------------------------
#
# PluginSheet - 表格预览插件（XLSX / ODS，只读）
#
# 只用 Qt 自带模块 + zlib：ZIP 容器由 Plugins/common/zipreader 处理，
# XML 用 QDomDocument 解析（需要 Qt6Xml.dll，部署见 plugin-deps.manifest）。
#
#-------------------------------------------------

QT       += core gui widgets xml

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = PluginSheet
TEMPLATE = lib

DEFINES += QT_DEPRECATED_WARNINGS

CONFIG += c++17

SOURCES += \
        mainwindow.cpp \
        pluginsheet.cpp \
        sheetdocument.cpp

HEADERS += \
        mainwindow.h \
        pluginsheet.h \
        sheetdocument.h

# ZIP 读取器来自 Plugins/common/（Book/Word/Sheet 共用同一份源码）
PLUGIN_SHARED_SOURCES = zipreader
LIBS += -lz

# 插件始终输出到主程序 exe 同级的 plugins/ 目录（自动适配 debug/release 与构建根目录）
# 公共设置（输出目录 + 插件元数据 sidecar：plugins/<DLL基名>.json）
PLUGIN_METADATA_JSON = $$PWD/pluginSheet.json
PLUGIN_CLASS_HEADER  = $$PWD/pluginsheet.h
include(../plugin_common.pri)

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

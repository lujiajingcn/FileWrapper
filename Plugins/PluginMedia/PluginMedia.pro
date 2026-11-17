#-------------------------------------------------
#
# PluginMedia - FFmpeg 8.1 video/audio player plugin
#
#-------------------------------------------------

QT       += widgets multimedia

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = PluginMedia
TEMPLATE = lib

# Emit warnings for deprecated Qt APIs
DEFINES += QT_DEPRECATED_WARNINGS

# Qt6 需要 C++17（Qt5 也可用），播放器代码使用了 lambda / 泛型等 C++11+ 特性
CONFIG += c++17

SOURCES += \
        mainwindow.cpp \
        pluginmedia.cpp

HEADERS += \
        mainwindow.h \
        pluginmedia.h

FORMS += \
        mainwindow.ui

INCLUDEPATH += $$PWD/include

LIBS += $$PWD/lib/libavcodec.dll.a \
        $$PWD/lib/libavdevice.dll.a \
        $$PWD/lib/libavfilter.dll.a \
        $$PWD/lib/libavformat.dll.a \
        $$PWD/lib/libavutil.dll.a \
        $$PWD/lib/libswresample.dll.a \
        $$PWD/lib/libswscale.dll.a

# 插件始终输出到主程序 exe 同级的 plugins/ 目录（自动适配 debug/release 与构建根目录）
# 公共设置（输出目录 + 插件元数据 sidecar：plugins/<DLL基名>.json）
PLUGIN_METADATA_JSON = $$PWD/pluginMedia.json
PLUGIN_CLASS_HEADER  = $$PWD/pluginmedia.h
include(../plugin_common.pri)

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

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

CONFIG += c++11

SOURCES += \
        main.cpp \
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

DESTDIR = ../../../build-FileWrapper-Desktop_Qt_5_13_0_MinGW_64_bit-Debug/FileWrapper/debug/plugins  # 输出目录

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

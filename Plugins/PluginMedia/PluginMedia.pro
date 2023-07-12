#-------------------------------------------------
#
# Project created by QtCreator 2023-07-09T09:39:23
#
#-------------------------------------------------

QT       += widgets multimedia

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = PluginMedia
TEMPLATE = lib

# The following define makes your compiler emit warnings if you use
# any feature of Qt which has been marked as deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if you use deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

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

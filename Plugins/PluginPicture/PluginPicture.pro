#-------------------------------------------------
#
# Project created by QtCreator 2023-07-03T09:59:10
#
#-------------------------------------------------

QT       += widgets

TARGET = PluginPicture
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

SOURCES += \
    mainwindow.cpp \
    pluginpicture.cpp

HEADERS += \
    mainwindow.h \
    pluginpicture.h

DESTDIR = ../../../build-FileWrapper-Desktop_Qt_5_13_0_MinGW_64_bit-Debug/FileWrapper/debug/plugins  # 输出目录

unix {
    target.path = /usr/lib
    INSTALLS += target
}

FORMS += \
    mainwindow.ui

#-------------------------------------------------
#
# Project created by QtCreator 2023-06-30T09:13:04
#
#-------------------------------------------------

QT       += core gui

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

CONFIG += c++11

SOURCES += \
        dlgabout.cpp \
        dlgmergefile.cpp \
        dlgoutputfile.cpp \
        dlgsplitfile.cpp \
        filemanager.cpp \
        main.cpp \
        mainwindow.cpp \
        pluginmanager.cpp

HEADERS += \
        dlgabout.h \
        dlgmergefile.h \
        dlgoutputfile.h \
        dlgsplitfile.h \
        filemanager.h \
        mainwindow.h \
        plugininterface.h \
        pluginmanager.h

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

# 部署 ffmpeg 运行库到 exe 同级目录，使 PluginMedia 视频插件能被 QPluginLoader 正确加载
# （PluginMedia.dll 链接 avcodec/avformat/avutil/swresample/swscale，这些 DLL 既不在 Qt 也不在 PATH 中，
#   缺失时 QPluginLoader 加载失败 -> 取插件接口为 null -> 程序报"获取插件接口失败"）
win32 {
    FFMPEG_BIN = $$PWD/../../ffmpeg-n8.1.2-34-g9b6c8969e0-win64-gpl-shared-8.1/ffmpeg-n8.1.2-34-g9b6c8969e0-win64-gpl-shared-8.1/bin
    CONFIG(debug, debug|release) {
        DST = $$OUT_PWD/debug
    } else {
        DST = $$OUT_PWD/release
    }
    QMAKE_POST_LINK = $$QMAKE_COPY \"$$FFMPEG_BIN/avcodec-62.dll\" \"$$DST\" || echo skip; \
        $$QMAKE_COPY \"$$FFMPEG_BIN/avformat-62.dll\" \"$$DST\" || echo skip; \
        $$QMAKE_COPY \"$$FFMPEG_BIN/avutil-60.dll\" \"$$DST\" || echo skip; \
        $$QMAKE_COPY \"$$FFMPEG_BIN/swresample-6.dll\" \"$$DST\" || echo skip; \
        $$QMAKE_COPY \"$$FFMPEG_BIN/swscale-9.dll\" \"$$DST\" || echo skip
}

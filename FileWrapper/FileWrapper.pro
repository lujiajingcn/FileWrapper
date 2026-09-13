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
        FFMPEG_DST = $$OUT_PWD/debug
    } else {
        FFMPEG_DST = $$OUT_PWD/release
    }

    # 部署这 5 个 DLL 有两条必须遵守的规则，任一出错都会让 PluginMedia.dll 加载失败
    # （缺 avformat/swresample/swscale → QPluginLoader 报"找不到指定的模块" → 取不到插件接口）：
    #
    # 规则 1：路径必须是 Windows 反斜杠形式。
    #   cmd.exe 的内置 copy 不认正斜杠路径，"copy /y "E:/a/b.dll" "E:/dst"" 会报
    #   「系统找不到指定的文件。已复制 0 个文件。」并且静默不报错。
    #   $$shell_path() 负责在 Windows 上把 / 归一化成 \（换机器/换盘符都不受影响）。
    #
    # 规则 2：每条 copy 必须独占一行。
    #   不能用 ";" 串联：";" 在 sh 里是命令分隔符，在 cmd.exe 里不是 ——
    #   用 ";" 会导致只有第 1 个 DLL 被拷贝，其余 4 个静默丢失。
    #   $$escape_expand(\\n\\t) 让 qmake 为每条命令生成独立的一行 recipe。
    FFMPEG_BIN = $$shell_path($$FFMPEG_BIN)
    FFMPEG_DST = $$shell_path($$FFMPEG_DST)

    QMAKE_POST_LINK = $$QMAKE_COPY \"$$shell_path($$FFMPEG_BIN/avcodec-62.dll)\" \"$$FFMPEG_DST\" || echo skip$$escape_expand(\\n\\t) \
        $$QMAKE_COPY \"$$shell_path($$FFMPEG_BIN/avformat-62.dll)\" \"$$FFMPEG_DST\" || echo skip$$escape_expand(\\n\\t) \
        $$QMAKE_COPY \"$$shell_path($$FFMPEG_BIN/avutil-60.dll)\" \"$$FFMPEG_DST\" || echo skip$$escape_expand(\\n\\t) \
        $$QMAKE_COPY \"$$shell_path($$FFMPEG_BIN/swresample-6.dll)\" \"$$FFMPEG_DST\" || echo skip$$escape_expand(\\n\\t) \
        $$QMAKE_COPY \"$$shell_path($$FFMPEG_BIN/swscale-9.dll)\" \"$$FFMPEG_DST\" || echo skip
}

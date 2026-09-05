#-------------------------------------------------
#
# PluginBook - 电子书预览插件（EPUB / FB2）
#
#-------------------------------------------------

QT       += core gui widgets xml

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = PluginBook
TEMPLATE = lib

DEFINES += QT_DEPRECATED_WARNINGS

CONFIG += c++11

# -------------------------------------------------
# libmobi（vendored 源码，静态库 libmobi.a）
# - 用内置 miniz.c 代替 zlib，用内置 xmlwriter 代替 libxml2，无外部依赖。
# - 因 miniz.c 的 amalgamation 模式（见 thirdparty/libmobi/Makefile.mingw 说明），
#   libmobi 用独立 Makefile 预编译为静态库，这里只负责链接，避免 qmake 把
#   miniz.c 误判为头文件依赖而丢弃导致链接失败。
# - 许可证：LGPL v3（静态链接保留署名与源码获取说明即可）。
# -------------------------------------------------
LIBMOBI_DIR = $$PWD/thirdparty/libmobi
INCLUDEPATH += $$LIBMOBI_DIR/src
DEFINES     += WITH_LIBMOBI
LIBS        += -L$$LIBMOBI_DIR -lmobi

# 链接 PluginBook.dll 前，先用独立 Makefile 构建 libmobi.a
libmobi.target    = $$LIBMOBI_DIR/libmobi.a
libmobi.commands  = $(MAKE) -C $$LIBMOBI_DIR -f Makefile.mingw
libmobi.depends   = $$files($$LIBMOBI_DIR/src/*.c) $$files($$LIBMOBI_DIR/src/*.h) $$LIBMOBI_DIR/Makefile.mingw
PRE_TARGETDEPS   += $$LIBMOBI_DIR/libmobi.a
QMAKE_EXTRA_TARGETS += libmobi

SOURCES += \
        main.cpp \
        mainwindow.cpp \
        pluginbook.cpp \
        zipreader.cpp

HEADERS += \
        mainwindow.h \
        pluginbook.h \
        zipreader.h

# 自包含的 ZIP 读取器（zipreader.cpp）用 zlib 解压 deflate 条目。
# 链接 MinGW 自带的静态 libz.a（无需额外部署 zlib1.dll）。
LIBS += -lz

FORMS += \
        mainwindow.ui

# 插件始终输出到主程序 exe 同级的 plugins/ 目录（自动适配 debug/release 与构建根目录）
debug:   DESTDIR = $$OUT_PWD/../../FileWrapper/debug/plugins
release: DESTDIR = $$OUT_PWD/../../FileWrapper/release/plugins  # 输出目录

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

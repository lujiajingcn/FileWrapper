#-------------------------------------------------
# PluginBook 最小自测（独立于主工程的测试程序）
# 直接复用插件真实源码 mainwindow.cpp/.h/.ui，
# 用 QZipWriter 现造 EPUB/FB2/MOBI/垃圾样本，
# 喂给 MainWindow::showBook 后检查渲染结果。
#-------------------------------------------------

QT       += core gui widgets gui-private xml

CONFIG += c++11 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET   = pluginbook_test

INCLUDEPATH += $$PWD/..
INCLUDEPATH += $$PWD/../../common
INCLUDEPATH += $$PWD/../thirdparty/libmobi/src
# test_main.cpp 用 Qt 私有 QZipWriter 造样本：Qt 6.9 里它在 QtCore/private，
# qmake 的 private 模块未自动加入版本化私有头目录，这里显式补上。
INCLUDEPATH += $$[QT_INSTALL_HEADERS]/QtCore/$$QT_VERSION
DEFINES     += WITH_LIBMOBI

SOURCES += \
    test_main.cpp \
    $$PWD/../../common/zipreader.cpp \
    $$PWD/../mainwindow.cpp

# zipreader.cpp 依赖 zlib（与 plugin_common.pri 的 PLUGIN_SHARED_SOURCES 一致）
LIBS += -lz

HEADERS += \
    $$PWD/../mainwindow.h

FORMS += \
    $$PWD/../mainwindow.ui

# libmobi 静态库：独立 Makefile 编出 libmobi.a，规避 qmake 把 miniz.c 误判为头文件
# 依赖而丢弃 miniz.o 导致的 mz_uncompress 未定义链接错误。
LIBMOBI_DIR = $$PWD/../thirdparty/libmobi
CONFIG(debug, debug|release) {
    LIBMOBI_OBJ = $$OUT_PWD/libmobi-obj-debug
    LIBMOBI_LIB = $$OUT_PWD/libmobi-lib-debug
} else {
    LIBMOBI_OBJ = $$OUT_PWD/libmobi-obj-release
    LIBMOBI_LIB = $$OUT_PWD/libmobi-lib-release
}
LIBS        += -L$$LIBMOBI_LIB -lmobi
libmobi.target   = $$LIBMOBI_LIB/libmobi.a
libmobi.commands = $(MAKE) -C \"$$shell_path($$LIBMOBI_DIR)\" -f Makefile.mingw OBJDIR=\"$$shell_path($$LIBMOBI_OBJ)\" LIBDIR=\"$$shell_path($$LIBMOBI_LIB)\"
libmobi.depends  = $$files($$LIBMOBI_DIR/src/*.c) $$files($$LIBMOBI_DIR/src/*.h) $$LIBMOBI_DIR/Makefile.mingw
PRE_TARGETDEPS  += $$LIBMOBI_LIB/libmobi.a
QMAKE_EXTRA_TARGETS += libmobi

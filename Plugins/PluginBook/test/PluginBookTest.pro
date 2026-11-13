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
INCLUDEPATH += $$PWD/../thirdparty/libmobi/src
DEFINES     += WITH_LIBMOBI

SOURCES += \
    test_main.cpp \
    $$PWD/../mainwindow.cpp

HEADERS += \
    $$PWD/../mainwindow.h

FORMS += \
    $$PWD/../mainwindow.ui

# libmobi 静态库：独立 Makefile 编出 libmobi.a，规避 qmake 把 miniz.c 误判为头文件
# 依赖而丢弃 miniz.o 导致的 mz_uncompress 未定义链接错误。
LIBMOBI_DIR = $$PWD/../thirdparty/libmobi
LIBS        += -L$$LIBMOBI_DIR -lmobi
libmobi.target   = $$LIBMOBI_DIR/libmobi.a
libmobi.commands = $(MAKE) -C $$LIBMOBI_DIR -f Makefile.mingw
libmobi.depends  = $$files($$LIBMOBI_DIR/src/*.c) $$files($$LIBMOBI_DIR/src/*.h) $$LIBMOBI_DIR/Makefile.mingw
PRE_TARGETDEPS  += $$LIBMOBI_DIR/libmobi.a
QMAKE_EXTRA_TARGETS += libmobi

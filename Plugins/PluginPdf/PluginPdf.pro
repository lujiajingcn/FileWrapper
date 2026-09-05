#-------------------------------------------------
#
# Project created by QtCreator 2023-07-09T09:39:23
#
#-------------------------------------------------

QT       += core gui

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = PluginPdf
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
        pluginpdf.cpp

HEADERS += \
        include/QtPdfium/qpdfium.h \
        include/QtPdfium/qpdfiumglobal.h \
        include/QtPdfium/qpdfiumpage.h \
        include/QtPdfium/qtpdfiumversion.h \
        mainwindow.h \
        pluginpdf.h

INCLUDEPATH += include/QtPdfium

LIBS += -L$$PWD/lib/ -lQt5Pdfium

FORMS += \
        mainwindow.ui

# 插件始终输出到主程序 exe 同级的 plugins/ 目录（自动适配 debug/release 与构建根目录）
debug:   DESTDIR = $$OUT_PWD/../../FileWrapper/debug/plugins
release: DESTDIR = $$OUT_PWD/../../FileWrapper/release/plugins  # 输出目录

# 部署 PDFium 运行库 Qt5Pdfium.dll 到 exe 同级目录，使 PluginPdf 能被 QPluginLoader 正确加载
# （PluginPdf.dll 链接 libQt5Pdfium，运行时需要 Qt5Pdfium.dll，缺失时 QPluginLoader 加载失败 -> 取插件接口为 null -> 报"获取插件接口失败"）
win32 {
    PDFIUM_SRC = $$PWD/lib/Qt5Pdfium.dll
    CONFIG(debug, debug|release): PDFIUM_DST = $$OUT_PWD/../../FileWrapper/debug
    else: PDFIUM_DST = $$OUT_PWD/../../FileWrapper/release
    QMAKE_POST_LINK = $$QMAKE_COPY \"$$PDFIUM_SRC\" \"$$PDFIUM_DST\" || echo skip
}

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

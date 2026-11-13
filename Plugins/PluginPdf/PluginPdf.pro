#-------------------------------------------------
# Project created by QtCreator 2023-07-09T09:39:23
#-------------------------------------------------

# Qt6 迁移：PDF 渲染改用 Qt 自带的 QtPdf 模块（QPdfDocument）。
# 原实现链接 Qt5 编译的 libQt5Pdfium.a / Qt5Pdfium.dll，后者导入 Qt5Core.dll
# 与 Qt5Gui.dll；本机升级到 Qt6 后这两个 DLL 不存在，QPluginLoader 加载
# PluginPdf.dll 时解析依赖链失败，报「找不到指定的模块」(ERROR_MOD_NOT_FOUND 126)。
QT       += core gui widgets pdf

TARGET = PluginPdf
TEMPLATE = lib

DEFINES += QT_DEPRECATED_WARNINGS

# Qt6 头文件要求 C++17
CONFIG += c++17

SOURCES += \
        main.cpp \
        mainwindow.cpp \
        pluginpdf.cpp

HEADERS += \
        mainwindow.h \
        pluginpdf.h

FORMS += \
        mainwindow.ui

# 插件始终输出到主程序 exe 同级的 plugins/ 目录（自动适配 debug/release 与构建根目录）
debug:   DESTDIR = $$OUT_PWD/../../FileWrapper/debug/plugins
release: DESTDIR = $$OUT_PWD/../../FileWrapper/release/plugins  # 输出目录

# 部署 Qt6Pdf.dll 及其运行依赖 Qt6Network.dll 到 exe 同级目录。
# Qt6Pdf 的 qmake 模块声明 QT.pdf.run_depends = network：Qt6Pdf.dll 运行时会加载
# Qt6Network.dll；Qt Creator 运行时 PATH 里有 Qt bin 所以能找到，
# 脱离 IDE 直接运行 exe 时必须把这两个 DLL 放在 exe 目录。
win32 {
    CONFIG(debug, debug|release): PDF_DST = $$OUT_PWD/../../FileWrapper/debug
    else: PDF_DST = $$OUT_PWD/../../FileWrapper/release

    # 路径必须经 $$shell_path() 归一化成 Windows 反斜杠形式：
    # cmd.exe 的内置 copy 不认正斜杠路径，且不返回错误码，会导致 DLL 静默没复制到。
    PDF_DST    = $$shell_path($$PDF_DST)
    QT_PDF_DLL = $$shell_path($$[QT_INSTALL_BINS]/Qt6Pdf.dll)
    QT_NET_DLL = $$shell_path($$[QT_INSTALL_BINS]/Qt6Network.dll)

    QMAKE_POST_LINK = $$QMAKE_COPY \"$$QT_PDF_DLL\" \"$$PDF_DST\" || echo skip$$escape_expand(\\n\\t) \
        $$QMAKE_COPY \"$$QT_NET_DLL\" \"$$PDF_DST\" || echo skip
}

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

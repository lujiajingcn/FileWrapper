# ============================================================================
# plugin_tests.pro —— 插件基础设施 + PluginWord 的端到端测试程序
#
# 直接把主程序的源文件编进测试程序（主程序、FileManager、PluginManager、
# PluginDiagnostics 都是"真的"），插件则用真实构建出来的 DLL 加载，
# 因此不需要先启动 GUI，也能覆盖到真实链路。
#
# 构建与运行请用 tests/run-plugin-tests.ps1（它会处理素材目录、插件目录、
# 日志与退出码），单独手动构建：
#     mkdir build\plugin_tests && cd build\plugin_tests
#     qmake ..\..\plugin_tests.pro -spec win32-g++ CONFIG+=debug
#     mingw32-make -j4
# ============================================================================

QT += core gui widgets
TEMPLATE = app
CONFIG += c++17 console
TARGET = plugin_tests

# 主程序源码
SRC = $$PWD/../FileWrapper
INCLUDEPATH += $$SRC
INCLUDEPATH += $$PWD/../include
VPATH += $$SRC

SOURCES += \
    $$SRC/builtinviewer.cpp \
    $$SRC/dlgabout.cpp \
    $$SRC/dlgmergefile.cpp \
    $$SRC/dlgoutputfile.cpp \
    $$SRC/dlgpluginmap.cpp \
    $$SRC/dlgsplitfile.cpp \
    $$SRC/filemanager.cpp \
    $$SRC/formatresolver.cpp \
    $$SRC/mainwindow.cpp \
    $$SRC/pluginmanager.cpp \
    $$SRC/plugindiag.cpp \
    $$PWD/plugin_tests.cpp

HEADERS += \
    $$SRC/archiveformat.h \
    $$SRC/builtinviewer.h \
    $$SRC/dlgabout.h \
    $$SRC/dlgmergefile.h \
    $$SRC/dlgoutputfile.h \
    $$SRC/dlgpluginmap.h \
    $$SRC/dlgsplitfile.h \
    $$SRC/filemanager.h \
    $$SRC/formatresolver.h \
    $$SRC/mainwindow.h \
    $$PWD/../include/plugininterface.h \
    $$SRC/pluginmanager.h \
    $$SRC/plugindiag.h

FORMS += \
    $$SRC/dlgabout.ui \
    $$SRC/dlgmergefile.ui \
    $$SRC/dlgoutputfile.ui \
    $$SRC/dlgsplitfile.ui \
    $$SRC/mainwindow.ui

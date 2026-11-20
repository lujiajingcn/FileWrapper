# ============================================================================
# core.pri —— 主程序核心源文件清单（主程序与 tests/plugin_tests.pro 共用）
#
# 以前 FileWrapper.pro 和 tests/plugin_tests.pro 各抄一份十几行的
# SOURCES/HEADERS/FORMS；新增/删除一个文件要改两处，漏一处就出现
# 「主程序能编、测试编不过」（2026-11-20 删除「插件映射」对话框时又碰到一次）。
#
# 用法：
#   * 主程序：include(core.pri) 后自己加 main.cpp
#   * 测试：  include(../FileWrapper/core.pri)（测试有自己的 main，不编 main.cpp）
#
# 注意：qmake 里带 include 的 .pri 中，$$PWD 指向**本文件所在目录**
# （FileWrapper/），所以路径不依赖调用方的工作目录。
# ============================================================================
FW_CORE = $$PWD

SOURCES += \
    $$FW_CORE/builtinviewer.cpp \
    $$FW_CORE/dlgabout.cpp \
    $$FW_CORE/dlgentrytags.cpp \
    $$FW_CORE/dlgfileproperty.cpp \
    $$FW_CORE/dlgmergefile.cpp \
    $$FW_CORE/dlgoutputfile.cpp \
    $$FW_CORE/dlgsplitfile.cpp \
    $$FW_CORE/filemanager.cpp \
    $$FW_CORE/filemetadata.cpp \
    $$FW_CORE/formatresolver.cpp \
    $$FW_CORE/mainwindow.cpp \
    $$FW_CORE/pluginmanager.cpp \
    $$FW_CORE/plugindiag.cpp

HEADERS += \
    $$FW_CORE/icons.h \
    $$FW_CORE/archiveformat.h \
    $$FW_CORE/builtinviewer.h \
    $$FW_CORE/dlgabout.h \
    $$FW_CORE/dlgentrytags.h \
    $$FW_CORE/dlgfileproperty.h \
    $$FW_CORE/dlgmergefile.h \
    $$FW_CORE/dlgoutputfile.h \
    $$FW_CORE/dlgsplitfile.h \
    $$FW_CORE/filemanager.h \
    $$FW_CORE/filemetadata.h \
    $$FW_CORE/formatresolver.h \
    $$FW_CORE/mainwindow.h \
    $$FW_CORE/pluginmanager.h \
    $$FW_CORE/plugindiag.h \
    $$FW_CORE/../include/plugininterface.h

FORMS += \
    $$FW_CORE/dlgabout.ui \
    $$FW_CORE/dlgmergefile.ui \
    $$FW_CORE/dlgoutputfile.ui \
    $$FW_CORE/dlgsplitfile.ui \
    $$FW_CORE/mainwindow.ui

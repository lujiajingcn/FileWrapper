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
    $$FW_CORE/dlgsettings.cpp \
    $$FW_CORE/dlgsplitfile.cpp \
    $$FW_CORE/filemanager.cpp \
    $$FW_CORE/filemetadata.cpp \
    $$FW_CORE/formatresolver.cpp \
    $$FW_CORE/mainwindow.cpp \
    $$FW_CORE/pluginmanager.cpp \
    $$FW_CORE/plugindiag.cpp

# ----------------------------------------------------------------------------
# 加密层（FWDA v5）：AES-256-GCM / HKDF（Windows CNG）+ Argon2id（vendored）
#
# 主程序与 tests/plugin_tests.pro 共用本清单；crypto_tests.pro 单独列源码。
# Argon2 使用固定版本 third_party/argon2（phc-winner-argon2 20190702，
# CC0/Apache-2.0），不依赖运行时 DLL，也不依赖网络。
# ----------------------------------------------------------------------------
ARGON2_DIR = $$FW_CORE/../third_party/argon2
INCLUDEPATH += $$ARGON2_DIR/include $$ARGON2_DIR/src $$ARGON2_DIR/src/blake2

SOURCES += \
    $$FW_CORE/crypto/cngcipher.cpp \
    $$FW_CORE/crypto/argonderive.cpp \
    $$FW_CORE/crypto/archivecrypto.cpp \
    $$FW_CORE/crypto/archiveindex.cpp \
    $$FW_CORE/crypto/keyring.cpp \
    $$ARGON2_DIR/src/argon2.c \
    $$ARGON2_DIR/src/core.c \
    $$ARGON2_DIR/src/encoding.c \
    $$ARGON2_DIR/src/ref.c \
    $$ARGON2_DIR/src/thread.c \
    $$ARGON2_DIR/src/blake2/blake2b.c

HEADERS += \
    $$FW_CORE/icons.h \
    $$FW_CORE/archiveformat.h \
    $$FW_CORE/builtinviewer.h \
    $$FW_CORE/dlgabout.h \
    $$FW_CORE/dlgentrytags.h \
    $$FW_CORE/dlgfileproperty.h \
    $$FW_CORE/dlgmergefile.h \
    $$FW_CORE/dlgoutputfile.h \
    $$FW_CORE/dlgsettings.h \
    $$FW_CORE/dlgsplitfile.h \
    $$FW_CORE/filemanager.h \
    $$FW_CORE/filemetadata.h \
    $$FW_CORE/formatresolver.h \
    $$FW_CORE/mainwindow.h \
    $$FW_CORE/pluginmanager.h \
    $$FW_CORE/plugindiag.h \
    $$FW_CORE/crypto/cngcipher.h \
    $$FW_CORE/crypto/argonderive.h \
    $$FW_CORE/crypto/archivecrypto.h \
    $$FW_CORE/crypto/archiveindex.h \
    $$FW_CORE/crypto/keyring.h \
    $$FW_CORE/../include/plugininterface.h

FORMS += \
    $$FW_CORE/dlgabout.ui \
    $$FW_CORE/dlgmergefile.ui \
    $$FW_CORE/dlgoutputfile.ui \
    $$FW_CORE/dlgsplitfile.ui \
    $$FW_CORE/mainwindow.ui

# 加密层依赖：Windows CNG（bcrypt）与 DPAPI（crypt32）
win32: LIBS += -lbcrypt -lcrypt32

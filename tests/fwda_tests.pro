# ============================================================================
# fwda_tests.pro —— FWDA v5 端到端测试（真实 FileManager + 加密）
#
# 覆盖：合并加密 → 打开解密 → 内容读取 → 校验 → 导出 → 分割 →
#       错误密钥拒绝 → 追加保存 → 只改头部保存 → 整理归档。
# 全程使用进程内测试密钥（KeyRing::setTestKey），不读写用户设置。
#
# 构建：
#   mkdir _bw\fwda_tests && cd _bw\fwda_tests
#   qmake ..\..\tests\fwda_tests.pro && mingw32-make -j6
# 运行：release\fwda_tests.exe
# ============================================================================

QT += core gui widgets
TEMPLATE = app
CONFIG += c++17 console
CONFIG -= app_bundle
TARGET = fwda_tests

SRC    = $$PWD/../FileWrapper
ARGON2 = $$PWD/../third_party/argon2

INCLUDEPATH += $$SRC $$ARGON2/include $$ARGON2/src $$ARGON2/src/blake2

SOURCES += fwda_tests.cpp \
    $$SRC/filemanager.cpp \
    $$SRC/filemetadata.cpp \
    $$SRC/dlgsettings.cpp \
    $$SRC/crypto/cngcipher.cpp \
    $$SRC/crypto/argonderive.cpp \
    $$SRC/crypto/archivecrypto.cpp \
    $$SRC/crypto/archiveindex.cpp \
    $$SRC/crypto/keyring.cpp \
    $$ARGON2/src/argon2.c \
    $$ARGON2/src/core.c \
    $$ARGON2/src/encoding.c \
    $$ARGON2/src/ref.c \
    $$ARGON2/src/thread.c \
    $$ARGON2/src/blake2/blake2b.c

HEADERS += \
    $$SRC/filemanager.h \
    $$SRC/filemetadata.h \
    $$SRC/dlgsettings.h \
    $$SRC/archiveformat.h \
    $$SRC/crypto/cngcipher.h \
    $$SRC/crypto/argonderive.h \
    $$SRC/crypto/archivecrypto.h \
    $$SRC/crypto/archiveindex.h \
    $$SRC/crypto/keyring.h

LIBS += -lbcrypt -lcrypt32

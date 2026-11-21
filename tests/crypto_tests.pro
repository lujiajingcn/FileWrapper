# ============================================================================
# crypto_tests.pro —— 加密层单测（CNG AES-GCM / HKDF / Argon2id）
#
# 构建：
#   mkdir _bw\crypto_tests && cd _bw\crypto_tests
#   qmake ..\..\tests\crypto_tests.pro
#   mingw32-make -j6
# 运行：crypto_tests.exe
#
# 只验证密码学原语与派生；不涉及归档格式（v5 的用例在后续阶段补）。
# ============================================================================

QT += core
QT -= gui
TEMPLATE = app
CONFIG += c++17 console
CONFIG -= app_bundle
TARGET = crypto_tests

SRC    = $$PWD/../FileWrapper
ARGON2 = $$PWD/../third_party/argon2

INCLUDEPATH += $$SRC
INCLUDEPATH += $$ARGON2/include $$ARGON2/src $$ARGON2/src/blake2

SOURCES += crypto_tests.cpp \
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
    $$SRC/crypto/cngcipher.h \
    $$SRC/crypto/argonderive.h \
    $$SRC/crypto/archivecrypto.h \
    $$SRC/crypto/keyring.h

LIBS += -lbcrypt -lcrypt32

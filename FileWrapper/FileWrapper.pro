#-------------------------------------------------
#
# Project created by QtCreator 2023-06-30T09:13:04
#
#-------------------------------------------------

QT       += core gui

# 鎻掍欢鎺ュ彛澶存斁鍦ㄤ粨搴撴牴 include/锛氫富绋嬪簭涓庢彃浠堕兘浠庨偅閲屽彇锛屾帴鍙ｅ崟涓€鏉ユ簮
INCLUDEPATH += $$_PRO_FILE_PWD_/../include

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = FileWrapper
TEMPLATE = app

# The following define makes your compiler emit warnings if you use
# any feature of Qt which has been marked as deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if you use deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

CONFIG += c++17

SOURCES += \
        builtinviewer.cpp \
        dlgabout.cpp \
        dlgmergefile.cpp \
        dlgoutputfile.cpp \
        dlgpluginmap.cpp \
        dlgsplitfile.cpp \
        filemanager.cpp \
        formatresolver.cpp \
        main.cpp \
        mainwindow.cpp \
        pluginmanager.cpp \
        plugindiag.cpp

HEADERS += \
        icons.h \
        archiveformat.h \
        builtinviewer.h \
        dlgabout.h \
        dlgmergefile.h \
        dlgoutputfile.h \
        dlgpluginmap.h \
        dlgsplitfile.h \
        filemanager.h \
        formatresolver.h \
        mainwindow.h \
        $$_PRO_FILE_PWD_/../include/plugininterface.h \
        pluginmanager.h \
        plugindiag.h

FORMS += \
        dlgabout.ui \
        dlgmergefile.ui \
        dlgoutputfile.ui \
        dlgsplitfile.ui \
        mainwindow.ui

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

# ============================================================================
# 鎻掍欢杩愯鏃朵緷璧栭儴缃诧紙2026-11-17 缁熶竴锛?#
# 浠ュ墠杩欓噷鍗曠嫭澶嶅埗 ffmpeg 鐨?5 涓?DLL锛孭luginPdf.pro 澶嶅埗 Qt6Pdf/Qt6Network锛?# PluginWord.pro 澶嶅埗 Qt6Xml 鈥斺€?涓夊闆舵暎銆佽矗浠讳笉娓咃紝鑰屼笖 Qt6Multimedia.dll
# 锛圥luginMedia 鐨勯煶棰戣緭鍑轰緷璧栵級鏍规湰娌′汉澶嶅埗锛氬紑鍙戞満 PATH 閲屾湁 Qt bin 鎵€浠ョ湅涓嶅嚭鏉ワ紝
# 鎹㈠彴鏈哄櫒灏便€屾彃浠跺姞杞藉け璐ャ€嶏紝绋嬪簭杩樹笉缁欎换浣曟彁绀恒€?#
# 鐜板湪锛氫緷璧栨竻鍗曚竴寰嬪啓鍦?deploy/plugin-deps.manifest锛堝敮涓€鏉ユ簮锛夛紝閾炬帴瀹屾垚鍚庣敱
# deploy/deploy-deps.ps1 缁熶竴澶嶅埗鍒?exe 鐩綍锛屽苟鐢熸垚 plugin-deploy-manifest.txt
# 渚涚▼搴忚嚜妫€锛堣 plugindiag.cpp锛夈€傛柊澧炴彃浠朵緷璧栧彧鏀?manifest锛屼笉瑕佸啀鍐?POST_LINK銆?# ============================================================================
RESOURCES += $$_PRO_FILE_PWD_/../resources/resources.qrc
win32: RC_FILE += $$_PRO_FILE_PWD_/../resources/appicon.rc

include(deploy.pri)

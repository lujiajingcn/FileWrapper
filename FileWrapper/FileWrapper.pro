#-------------------------------------------------
#
# Project created by QtCreator 2023-06-30T09:13:04
#
#-------------------------------------------------

QT       += core gui

# 閹绘帊娆㈤幒銉ュ經婢跺瓨鏂侀崷銊ょ波鎼存挻鐗?include/閿涙矮瀵岀粙瀣碍娑撳孩褰冩禒鍫曞厴娴犲酣鍋呴柌灞藉絿閿涘本甯撮崣锝呭礋娑撯偓閺夈儲绨?INCLUDEPATH += $$PWD/../include

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
        $$PWD/../include/plugininterface.h \
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
# 閹绘帊娆㈡潻鎰攽閺冩湹绶风挧鏍劥缂冭绱?026-11-17 缂佺喍绔撮敍?#
# 娴犮儱澧犳潻娆撳櫡閸楁洜瀚径宥呭煑 ffmpeg 閻?5 娑?DLL閿涘luginPdf.pro 婢跺秴鍩?Qt6Pdf/Qt6Network閿?# PluginWord.pro 婢跺秴鍩?Qt6Xml 閳ユ柡鈧?娑撳顦╅梿鑸垫殠閵嗕浇鐭楁禒璁崇瑝濞撳拑绱濋懓灞肩瑬 Qt6Multimedia.dll
# 閿涘湧luginMedia 閻ㄥ嫰鐓舵０鎴ｇ翻閸戣桨绶风挧鏍电礆閺嶈婀板▽鈥叉眽婢跺秴鍩楅敍姘磻閸欐垶婧€ PATH 闁插本婀?Qt bin 閹碘偓娴犮儳婀呮稉宥呭毉閺夈儻绱?# 閹广垹褰撮張鍝勬珤鐏忎究鈧本褰冩禒璺哄鏉炶棄銇戠拹銉ｂ偓宥忕礉缁嬪绨潻妯圭瑝缂佹瑤鎹㈡担鏇熷絹缁€鎭掆偓?#
# 閻滄澘婀敍姘贩鐠ф牗绔婚崡鏇氱瀵板鍟撻崷?deploy/plugin-deps.manifest閿涘牆鏁稉鈧弶銉︾爱閿涘绱濋柧鐐复鐎瑰本鍨氶崥搴ｆ暠
# deploy/deploy-deps.ps1 缂佺喍绔存径宥呭煑閸?exe 閻╊喖缍嶉敍灞借嫙閻㈢喐鍨?plugin-deploy-manifest.txt
# 娓氭稓鈻兼惔蹇氬殰濡偓閿涘牐顫?plugindiag.cpp閿涘鈧倹鏌婃晶鐐村絻娴犳湹绶风挧鏍у涧閺€?manifest閿涘奔绗夌憰浣稿晙閸?POST_LINK閵?# ============================================================================
RESOURCES += $$PWD/../resources/resources.qrc
win32: RC_FILE += $$PWD/../resources/appicon.rc

include(deploy.pri)

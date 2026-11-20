#include "mainwindow.h"
#include "icons.h"
#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QFont>
#include <QLibraryInfo>
#include <QStyleFactory>
#include <QTranslator>

// 载入界面主题：整个程序的观感（配色/圆角/间距/行高）都由这份 QSS 决定。
// 深色模式（P1-6）后续接入时在这里按系统主题切换即可，样式另存一份 qss。
static void applyTheme(QApplication &app)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QFont font(QStringLiteral("Microsoft YaHei UI"));
    font.setPointSizeF(10.0);
    app.setFont(font);

    QFile qss(QStringLiteral(":/theme/light.qss"));
    if (qss.open(QIODevice::ReadOnly))
        app.setStyleSheet(QString::fromUtf8(qss.readAll()));
}

// Qt 自带控件（标准按钮、输入框右键菜单等）的中文化。
// 发布时把 Qt 的 qtbase_zh_CN.qm 放进 exe 同级 translations/ 目录；
// 开发机上则回落到 Qt 安装目录。两者都找不到时保持英文，不影响功能。
static void installChineseTranslator(QApplication &app)
{
    auto *pTranslator = new QTranslator(&app);
    const QStringList dirs = {
        QCoreApplication::applicationDirPath() + QStringLiteral("/translations"),
        QLibraryInfo::path(QLibraryInfo::TranslationsPath)
    };
    for (const QString &sDir : dirs)
    {
        if (pTranslator->load(QStringLiteral("qtbase_zh_CN"), sDir))
        {
            app.installTranslator(pTranslator);
            return;
        }
    }
    qWarning("[i18n] 未找到 qtbase_zh_CN.qm，标准按钮将显示英文");
}

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    applyTheme(a);
    installChineseTranslator(a);

    a.setWindowIcon(FwIcons::appIcon());
    MainWindow w;
    w.show();

    return a.exec();
}

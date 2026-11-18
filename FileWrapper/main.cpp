#include "mainwindow.h"
#include "icons.h"
#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    a.setWindowIcon(FwIcons::get(QStringLiteral("appicon")));
    MainWindow w;
    w.show();

    return a.exec();
}

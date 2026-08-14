#include "mainwindow.h"
#include "ui_mainwindow.h"

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::showText(char *szBuf, qint64 nFileLen)
{
    if (szBuf == nullptr || nFileLen <= 0)
        return;

    // 使用 nFileLen 作为长度（而非 strlen），避免读取越界或 NUL 截断
    QByteArray array(szBuf, (int)nFileLen);
    QString str(array);
    ui->textEdit->setText(str);
}

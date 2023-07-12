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

void MainWindow::showText(char *szBuf, int nFileLen)
{
    QByteArray array;
    array.resize(strlen(szBuf));//重置数据大小
    memcpy(array.data(), szBuf, strlen(szBuf));//copy数据
    QString str;
    str.prepend(array);
    ui->textEdit->setText(str);
}

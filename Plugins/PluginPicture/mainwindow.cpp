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

void MainWindow::showPicture(char *szBuf, int nFileLen)
{
    QPixmap pix;
    pix.loadFromData(reinterpret_cast<uchar*>(szBuf), nFileLen);
    int nHeight = this->height();
    pix = pix.scaledToHeight(nHeight);
    ui->labelPicture->setPixmap(pix);
}

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QDebug>
#include <QTransform>

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

void MainWindow::showPicture(char *szBuf, qint64 nFileLen)
{
    if (szBuf == nullptr || nFileLen <= 0)
        return;

    QPixmap pix;
    if (!pix.loadFromData(reinterpret_cast<uchar*>(szBuf), (int)nFileLen))
    {
        // 图片解码失败：给出提示，避免静默显示空白
        ui->labelPicture->setText("无法加载图片：数据已损坏或格式不受支持。");
        ui->labelPicture->setPixmap(QPixmap());
        m_originalPix = QPixmap();
        return;
    }
    m_originalPix = pix;

    // 初始缩放：适配窗口高度；窗口尚未显示时用原图尺寸保底，避免大图溢出
    m_scale = 1.0;
    if (!m_originalPix.isNull() && m_originalPix.height() > 0)
    {
        int availH = ui->scrollArea->viewport()->height();
        if (availH > 0)
            m_scale = (qreal)availH / m_originalPix.height();
        else
            m_scale = (qreal)qMin(this->height(), 600) / m_originalPix.height();
        if (m_scale <= 0.0) m_scale = 1.0;
    }
    m_rotateDeg = 0;
    applyTransform();
}

void MainWindow::applyTransform()
{
    if (m_originalPix.isNull())
        return;

    QPixmap rotated = m_originalPix;
    // 先旋转
    if (m_rotateDeg == 90)
        rotated = m_originalPix.transformed(QTransform().rotate(90), Qt::SmoothTransformation);
    else if (m_rotateDeg == 180)
        rotated = m_originalPix.transformed(QTransform().rotate(180), Qt::SmoothTransformation);
    else if (m_rotateDeg == 270)
        rotated = m_originalPix.transformed(QTransform().rotate(270), Qt::SmoothTransformation);

    // 再缩放
    qreal w = rotated.width() * m_scale;
    qreal h = rotated.height() * m_scale;
    if (w <= 0 || h <= 0)
        return;
    QPixmap scaled = rotated.scaled((int)w, (int)h, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    ui->labelPicture->setPixmap(scaled);
    ui->labelPicture->adjustSize();
}

void MainWindow::on_actZoomIn_triggered()
{
    m_scale *= 1.25;
    if (m_scale > 8.0) m_scale = 8.0;   // 最多放大 8 倍
    applyTransform();
}

void MainWindow::on_actZoomOut_triggered()
{
    m_scale /= 1.25;
    if (m_scale < 0.1) m_scale = 0.1;   // 最多缩小到 10%
    applyTransform();
}

void MainWindow::on_actRotate_triggered()
{
    m_rotateDeg = (m_rotateDeg + 90) % 360;
    applyTransform();
}

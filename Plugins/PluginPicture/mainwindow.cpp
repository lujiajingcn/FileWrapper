#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QDebug>
#include <QTransform>
#include <limits>
#include <cmath>

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    // 两个常用视图动作：适应窗口 / 实际大小（不改 .ui，直接追加到工具栏）
    QAction *pFit = new QAction(tr("适应窗口"), this);
    pFit->setToolTip(tr("按窗口大小显示整张图片"));
    QAction *pActual = new QAction(tr("实际大小"), this);
    pActual->setToolTip(tr("按 100% 原始像素显示"));
    ui->toolBar->addAction(pFit);
    ui->toolBar->addAction(pActual);
    connect(pFit, &QAction::triggered, this, &MainWindow::on_actFit_triggered);
    connect(pActual, &QAction::triggered, this, &MainWindow::on_actActualSize_triggered);
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::clearContent()
{
    m_originalPix = QPixmap();
    m_scale = 1.0;
    m_rotateDeg = 0;
    // QLabel::clear() 同时清掉文本与 pixmap
    ui->labelPicture->clear();
}

void MainWindow::showPicture(char *szBuf, qint64 nFileLen)
{
    if (szBuf == nullptr || nFileLen <= 0)
        return;
    // QPixmap::loadFromData(const uchar*, uint, ...) 的长度形参是 uint
    // （本机 Qt 6.8.3 的 qpixmap.h:97 实测只有这一个重载），不是 qsizetype。
    // 64 位下 qsizetype 与 qint64 同宽，原「超过 qsizetype::max()」判断恒为
    // false（死代码）；按 uint 的真实范围显式判断，避免隐式截断成错误长度。
    if (nFileLen > qint64(std::numeric_limits<uint>::max()))
    {
        // 同样先清 pixmap 再写提示：QLabel::setPixmap() 会清除文本，
        // 反之 setText 也会清掉 pixmap，顺序不能反。
        ui->labelPicture->setPixmap(QPixmap());
        ui->labelPicture->setText("文件过大，无法载入内存显示。");
        m_originalPix = QPixmap();
        return;
    }

    QPixmap pix;
    if (!pix.loadFromData(reinterpret_cast<const uchar*>(szBuf), static_cast<uint>(nFileLen)))
    {
        // 图片解码失败：给出提示，避免静默显示空白。
        // 注意顺序：QLabel::setPixmap() 会清除已设置的文本，
        // 因此必须先 setPixmap(空) 清掉旧图，再 setText 写提示文案。
        ui->labelPicture->setPixmap(QPixmap());
        ui->labelPicture->setText("无法加载图片：数据已损坏或格式不受支持。");
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

    // 连续放大（最高 8x）时 QPixmap 会按 4 字节/像素一次性分配，超大图可能
    // 吃掉数 GB 内存。按总像素夹一个上限，保持宽高比等比缩小。
    constexpr qreal kMaxPixels = 64.0 * 1024 * 1024;   // 64M 像素 ≈ 256 MiB (ARGB32)
    if (w * h > kMaxPixels)
    {
        const qreal k = std::sqrt(kMaxPixels / (w * h));
        w *= k;
        h *= k;
    }
    QPixmap scaled = rotated.scaled(qMax(1, qRound(w)), qMax(1, qRound(h)),
                                    Qt::KeepAspectRatio, Qt::SmoothTransformation);
    ui->labelPicture->setPixmap(scaled);
    ui->labelPicture->adjustSize();
}

void MainWindow::on_actFit_triggered()
{
    if (m_originalPix.isNull())
        return;

    // 旋转 90/270 时宽高互换后再算适应比例
    QSize orig = m_originalPix.size();
    if (m_rotateDeg == 90 || m_rotateDeg == 270)
        orig.transpose();

    const QSize avail = ui->labelPicture->contentsRect().size();
    if (avail.width() <= 0 || avail.height() <= 0 || orig.isEmpty())
        return;

    m_scale = qMin(qreal(avail.width()) / orig.width(),
                   qreal(avail.height()) / orig.height());
    m_scale = qBound(0.1, m_scale, 8.0);
    applyTransform();
}

void MainWindow::on_actActualSize_triggered()
{
    if (m_originalPix.isNull())
        return;
    m_scale = 1.0;
    applyTransform();
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

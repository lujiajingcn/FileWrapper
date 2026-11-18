#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QDebug>
#include <QBuffer>
#include <QImageReader>
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
    connect(pFit, &QAction::triggered, this, &MainWindow::fitToWindow);
    connect(pActual, &QAction::triggered, this, &MainWindow::actualSize);
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

    // QPixmap::loadFromData 不会应用 EXIF Orientation（Qt 6.9 默认
    // autoTransform=false）；手机竖拍照片会横躺/镜像。用 QImageReader 显式转正。
    QImage img;
    {
        QBuffer buf;
        buf.setData(szBuf, static_cast<int>(nFileLen));
        buf.open(QIODevice::ReadOnly);
        QImageReader reader(&buf);
        reader.setAutoTransform(true);
        img = reader.read();
    }
    QPixmap pix = QPixmap::fromImage(img);
    if (img.isNull())
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
        // 与放大/缩小按钮统一到 [0.1,8]；否则 16x16 小图标可初始 30 倍，
        // 点“放大”反而被钳到 8 倍变小。
        m_scale = qBound(0.1, m_scale, 8.0);
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

void MainWindow::fitToWindow()
{
    if (m_originalPix.isNull())
        return;

    // 旋转 90/270 时宽高互换后再算适应比例
    QSize orig = m_originalPix.size();
    if (m_rotateDeg == 90 || m_rotateDeg == 270)
        orig.transpose();

    // 注意：必须用 QScrollArea 的视口尺寸，不能用 labelPicture 的尺寸。
    // label 在 applyTransform() 里会被 adjustSize() 成"当前缩放后的图片大小"，
    // 拿它当可用区域算出来的比例恒等于当前缩放，按钮看起来就没有效果。
    const QSize avail = ui->scrollArea->viewport()->size();
    if (avail.isEmpty() || orig.isEmpty())
        return;

    const int nMargin = 8;   // 预留边距，避免刚好贴边/触发滚动条抖动
    m_scale = qMin(qreal(avail.width() - nMargin) / orig.width(),
                   qreal(avail.height() - nMargin) / orig.height());
    if (m_scale <= 0.0)
        return;
    m_scale = qBound(0.1, m_scale, 8.0);
    applyTransform();
}

void MainWindow::actualSize()
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

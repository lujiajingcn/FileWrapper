#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QDebug>
#include <QBuffer>
#include <QImageReader>
#include <QTransform>
#include <QStatusBar>
#include <limits>
#include <cmath>

// P1-22：解码前的大小预算。64 MP 是插件原有的显示上限；256 MiB 是按 ARGB32
// （4 字节/像素）估算的解码内存上限，两者取更严格者。
static constexpr qint64 kMaxDecodePixels    = 64000000;                 // 6400 万像素
static constexpr qint64 kMaxDecodeRgbaBytes = 256LL * 1024 * 1024;      // 256 MiB
// 超过 40 MP 且比视口大时，初始预览直接让解码器按视口尺寸输出，避免全分辨率中间图。
static constexpr qint64 kPreviewDecodePixels = 40LL * 1024 * 1024;      // 40 MP

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
    m_bPreviewScaled = false;
    m_declaredSize = QSize();
    // QLabel::clear() 同时清掉文本与 pixmap
    ui->labelPicture->clear();
    statusBar()->clearMessage();
}

// P1-22：统一的错误显示。QLabel 的文本与 pixmap 互斥，必须先清图再 setText，
// 否则旧图片会残留或提示被立刻清掉。
void MainWindow::showError(const QString &msg)
{
    ui->labelPicture->setPixmap(QPixmap());
    ui->labelPicture->setText(msg);
    m_originalPix = QPixmap();
    m_bPreviewScaled = false;
    m_declaredSize = QSize();
    statusBar()->clearMessage();
}

// P1-22：初始预览的可用视口尺寸；窗口尚未布局时用窗口尺寸保底，并预留边距。
QSize MainWindow::previewViewportSize() const
{
    QSize avail = ui->scrollArea->viewport()->size();
    if (avail.isEmpty())
        avail = QSize(qMin(width(), 800), qMin(height(), 600));
    avail -= QSize(8, 8);   // 与 fitToWindow() 的边距一致，避免刚贴边
    return avail.expandedTo(QSize(1, 1));
}

void MainWindow::showPicture(char *szBuf, qint64 nFileLen)
{
    if (szBuf == nullptr || nFileLen <= 0)
        return;
    // QBuffer/QByteArray 的长度形参是 qsizetype；这里仍按 2GiB 设一个实际可用上限
    // （超大图不可能在内存里解码显示），同时避免把长度窄化成 int 后变成负数
    // （旧实现按 uint 上限判断、却按 int 传参，2~4GiB 会变成负值）。
    if (nFileLen > qint64(std::numeric_limits<int>::max()))
    {
        showError(QStringLiteral("文件过大，无法载入内存显示。"));
        return;
    }

    // QPixmap::loadFromData 不会应用 EXIF Orientation（Qt 6.9 默认
    // autoTransform=false）；手机竖拍照片会横躺/镜像。用 QImageReader 显式转正。
    m_bPreviewScaled = false;
    m_declaredSize = QSize();
    QImage img;
    {
        QBuffer buf;
        buf.setData(szBuf, qsizetype(nFileLen));
        if (!buf.open(QIODevice::ReadOnly))
        {
            showError(QStringLiteral("无法打开图片数据。"));
            return;
        }
        QImageReader reader(&buf);
        reader.setAutoTransform(true);

        // P1-22：先读头部声明的尺寸做预算，超过 64 MP / 256 MiB 直接拒绝，
        // 不再把必定失败的图片交给解码器去分配内存。
        const QSize declared = reader.size();
        if (declared.isValid())
        {
            // 先算像素数再与 budget/4 比较，避免恶意宽高让 width*height*4
            // 溢出 qint64（64 MP 的 RGBA 估算恰好是 256 MiB）。
            const qint64 pixels = qint64(declared.width()) * qint64(declared.height());
            const bool bTooManyPixels = (pixels > kMaxDecodePixels);
            const bool bTooManyBytes  = (pixels > kMaxDecodeRgbaBytes / 4);
            if (bTooManyPixels || bTooManyBytes)
            {
                qWarning() << "[PluginPicture] 图片尺寸超过解码预算，拒绝解码:" << declared;
                showError(tr("图片尺寸过大（%1×%2，约 %3 MP），超过 64 MP / 256 MiB "
                             "解码上限，无法显示。")
                              .arg(declared.width())
                              .arg(declared.height())
                              .arg(QString::number(qreal(pixels) / 1e6, 'f', 1)));
                return;
            }

            // P1-22：初始预览 —— 大图（>40 MP）且比视口大时按视口尺寸解码，
            // 避免先解出全分辨率 QImage 再缩放（峰值内存可降一个数量级）。
            const QSize avail = previewViewportSize();
            // 视口尚未完成布局时可能只有几十像素：给预览一个下限，避免把大图
            // 解成缩略图后即使放大也再没有细节（仍然远小于全分辨率中间图）。
            const QSize previewBox = avail.expandedTo(QSize(640, 480));
            if (pixels > kPreviewDecodePixels
                && (declared.width() > previewBox.width() || declared.height() > previewBox.height()))
            {
                const QSize target = declared.scaled(previewBox, Qt::KeepAspectRatio);
                if (target.isValid() && target.width() > 0 && target.height() > 0)
                {
                    // 支持 ScaledSize 的格式（JPEG 等）解码时直接降采样，不产生全分辨率
                    // 中间图；个别格式不支持时 Qt 会在解码后再缩放，常驻内存仍按预览尺寸。
                    reader.setScaledSize(target);
                    m_declaredSize = declared;
                    m_bPreviewScaled = true;
                }
            }
        }
        img = reader.read();
    }

    if (img.isNull())
    {
        // 图片解码失败：给出提示，避免静默显示空白。
        showError(QStringLiteral("无法加载图片：数据已损坏或格式不受支持。"));
        return;
    }

    // 部分格式（或畸形文件）拿不到头部尺寸：解码后再做同样的预算兜底，
    // 至少不把超限图片留在界面上。
    {
        const qint64 decodedPixels = qint64(img.width()) * qint64(img.height());
        const bool bTooManyPixels = (decodedPixels > kMaxDecodePixels);
        const bool bTooManyBytes  = (decodedPixels > kMaxDecodeRgbaBytes / 4);
        if (bTooManyPixels || bTooManyBytes)
        {
            qWarning() << "[PluginPicture] 解码后尺寸超过预算，拒绝显示:" << img.size();
            showError(tr("图片尺寸过大（%1×%2），超过 64 MP / 256 MiB 解码上限，无法显示。")
                          .arg(img.width())
                          .arg(img.height()));
            return;
        }
    }

    m_originalPix = QPixmap::fromImage(img);

    // 初始缩放：适配窗口高度；窗口尚未显示时用原图尺寸保底，避免大图溢出
    m_scale = 1.0;
    if (m_bPreviewScaled)
    {
        // 解码器已按视口缩好，直接 1:1 显示，避免再插值一次；
        // 若个别格式忽略了 setScaledSize（仍解出全分辨率），退回按视口适配，
        // 不把全分辨率图 1:1 铺开（P1-22 兜底）。
        const QSize avail = previewViewportSize();
        const bool bStillTooBig = m_originalPix.width() > avail.width()
                                  || m_originalPix.height() > avail.height();
        m_scale = bStillTooBig
                      ? qMin(qMin(qreal(avail.width()) / m_originalPix.width(),
                                  qreal(avail.height()) / m_originalPix.height()), 8.0)
                      : 1.0;
        if (m_scale <= 0.0) m_scale = 1.0;
    }
    else if (!m_originalPix.isNull() && m_originalPix.height() > 0)
    {
        int availH = ui->scrollArea->viewport()->height();
        if (availH > 0)
            m_scale = (qreal)availH / m_originalPix.height();
        else
            m_scale = (qreal)qMin(this->height(), 600) / m_originalPix.height();
        if (m_scale <= 0.0) m_scale = 1.0;
        // P2-13：初始比例属于"适应窗口"计算，允许小于 10%（大图不该被强制放大到 10%
        // 而溢出视口）；只保留 8 倍上限，避免 16x16 小图标初始 30 倍。
        m_scale = qMin(m_scale, 8.0);
    }
    m_rotateDeg = 0;
    applyTransform();
}

void MainWindow::applyTransform()
{
    if (m_originalPix.isNull())
        return;

    // P2-13：始终从"原图 + 当前 m_scale"重新计算，不缓存任何中间缩放图，
    // 缩放/旋转来回切换时不会因为旧状态跳变。
    const QSize srcSize = m_originalPix.size();
    const bool bQuarterTurn = (m_rotateDeg == 90 || m_rotateDeg == 270);
    // 旋转 90/270 后显示尺寸的宽高互换
    const QSize shownSize = bQuarterTurn ? srcSize.transposed() : srcSize;

    qreal w = shownSize.width() * m_scale;
    qreal h = shownSize.height() * m_scale;
    if (w <= 0 || h <= 0)
        return;

    // 连续放大（最高 8x）时 QPixmap 会按 4 字节/像素一次性分配，超大图可能
    // 吃掉数 GB 内存。按总像素夹一个上限，保持宽高比等比缩小；同时把实际
    // 生效的比例写回 m_scale（P2-13：否则下次缩放仍按未封顶的比例计算，画面跳变）。
    constexpr qreal kMaxPixels = 64.0 * 1024 * 1024;   // 64M 像素 ≈ 256 MiB (ARGB32)
    bool bPixelCapped = false;
    if (w * h > kMaxPixels + 0.5)
    {
        const qreal k = std::sqrt(kMaxPixels / (w * h));
        m_scale *= k;
        w = shownSize.width() * m_scale;
        h = shownSize.height() * m_scale;
        bPixelCapped = true;
    }
    const int nTargetW = qMax(1, qRound(w));
    const int nTargetH = qMax(1, qRound(h));

    QPixmap scaled;
    // P1-22：90/270 度旋转不再对整幅原图做旋转中间图 —— 原图比显示尺寸大时，
    // 先缩到显示尺寸再旋转。旋转前的目标尺寸是旋转后的宽高互换（th × tw）。
    if (bQuarterTurn && qint64(srcSize.width()) * srcSize.height() > qint64(nTargetW) * nTargetH)
    {
        const QPixmap pre = m_originalPix.scaled(qMax(1, nTargetH), qMax(1, nTargetW),
                                                 Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        scaled = pre.transformed(QTransform().rotate(m_rotateDeg), Qt::SmoothTransformation);
    }
    else
    {
        QPixmap rotated = m_originalPix;
        if (m_rotateDeg != 0)
            rotated = m_originalPix.transformed(QTransform().rotate(m_rotateDeg),
                                                Qt::SmoothTransformation);
        scaled = rotated.scaled(nTargetW, nTargetH, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    ui->labelPicture->setPixmap(scaled);
    ui->labelPicture->adjustSize();

    // P2-13：状态栏同步显示"实际生效"的缩放比例（像素上限修正后 m_scale 已是实际值）
    QString status = tr("缩放 %1%  |  %2×%3 像素")
                         .arg(qRound(m_scale * 100.0))
                         .arg(scaled.width())
                         .arg(scaled.height());
    if (bPixelCapped)
        status += tr("  |  已按 64 MP 像素上限缩小显示");
    if (m_bPreviewScaled)
        status += tr("  |  预览解码自 %1×%2")
                      .arg(m_declaredSize.width())
                      .arg(m_declaredSize.height());
    statusBar()->showMessage(status);
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
    // P2-13：适应窗口允许任何正比例（大图不再被强制放大到 10% 而溢出视口）；
    // 10% 下限只保留给手动缩小按钮。
    m_scale = qMin(m_scale, 8.0);
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
    // P2-13：手动缩小的下限仍是 10%；但若当前比例来自"适应窗口"且小于 10%，
    // 继续缩小而不是被钳回 10%（否则点"缩小"反而会变大）。
    constexpr qreal kMinManualScale = 0.1;
    const qreal next = m_scale / 1.25;
    m_scale = (m_scale >= kMinManualScale) ? qMax(kMinManualScale, next) : next;
    applyTransform();
}

void MainWindow::on_actRotate_triggered()
{
    m_rotateDeg = (m_rotateDeg + 90) % 360;
    applyTransform();
}

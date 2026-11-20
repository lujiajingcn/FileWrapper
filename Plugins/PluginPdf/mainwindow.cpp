#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QDebug>
#include <QImage>
#include <QPixmap>
#include <QPainter>
#include <QColor>
#include <QFont>

namespace {
// QPdfDocument 的错误码 -> 用户能照做的中文说明。
// 以前这些原因只进 qWarning，界面只剩「空文档」，用户分不清"文件是空的"
// 和"文件损坏/加密/读不到"。
QString pdfErrorText(QPdfDocument::Error err)
{
    switch (err)
    {
    case QPdfDocument::Error::InvalidFileFormat:
        return QStringLiteral("不是有效的 PDF 文件（文件已损坏，或扩展名与实际内容不符）。");
    case QPdfDocument::Error::IncorrectPassword:
        return QStringLiteral("该 PDF 已加密，需要密码才能打开；当前查看器不支持输入密码。");
    case QPdfDocument::Error::UnsupportedSecurityScheme:
        return QStringLiteral("该 PDF 使用了不支持的加密/安全方案，无法显示。");
    case QPdfDocument::Error::FileNotFound:
        return QStringLiteral("PDF 文件不存在或无法读取。");
    case QPdfDocument::Error::DataNotYetAvailable:
        return QStringLiteral("PDF 数据尚未就绪，无法显示。");
    case QPdfDocument::Error::None:
    case QPdfDocument::Error::Unknown:
        break;
    }
    return QStringLiteral("无法解析该 PDF（文件可能已损坏）。");
}
} // namespace

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    m_lbContent = ui->label;
    m_nCurPage = 0;
    m_nPageCount = 0;
    m_nScale = 100;

    //页码
    m_lePageNum = new QLineEdit;
    m_lePageNum->setMaximumWidth(72);
    m_lePageNum->setMaxLength(11);
    ui->mainToolBar->insertWidget(ui->actionLastPage,(QWidget *)m_lePageNum);
    connect(m_lePageNum, &QLineEdit::returnPressed, this, &MainWindow::gotoPage);

    // P2-10：初始就是"空文档"状态：翻页按钮不可用、页码显示"空文档"而不是 1/0，
    // 内容区也不再显示 .ui 里的占位文字 "TextLabel"。
    m_lbContent->setText(tr("空文档"));
    updatePageActions();
    setLineEditPageNum();
}

MainWindow::~MainWindow()
{
    delete m_lePageNum;
    delete ui;
}

void MainWindow::clearContent()
{
    // 成员声明顺序保证 m_pdfBuf 在 m_doc.close() 时仍然有效
    m_doc.close();
    m_pdfBuf.close();
    m_pdfBuf.setData(QByteArray());
    m_nPageCount = 0;
    m_nCurPage = 0;
    m_lastError.clear();   // 每次打开尝试/清屏都清掉上一次的失败原因
    // P2-10：0 页时给出明确的"空文档"文案，并禁用翻页动作；
    // 页码框同样显示"空文档"，避免出现 "1/0" 这种无意义的页码。
    if (m_lbContent) m_lbContent->setText(tr("空文档"));
    updatePageActions();
    setLineEditPageNum();
}

// 打开失败：清掉旧画面并把中文原因留在内容区（插件层 lastError() 同时回报宿主）
void MainWindow::showLoadError(const QString &msg)
{
    m_lastError = msg;
    if (!m_lbContent) return;
    m_lbContent->setPixmap(QPixmap());   // QLabel 的 pixmap 与 text 互斥，先清图再写字
    m_lbContent->setText(msg);
}

bool MainWindow::showPdf(char *szBuf, qint64 nFileLen)
{
    // 换文件先清掉上一份的画面/页码/文档：任何失败路径都不会残留旧 PDF 页面。
    // 原实现只在 load 失败时 qWarning，而 updatePage() 在 m_nPageCount<=0 时
    // 直接 return，导致界面仍显示上一个文件的内容。
    clearContent();

    if (szBuf == nullptr || nFileLen <= 0)
        return false;

    // 主程序会在 sendFileData 返回后释放原始缓冲区，因此必须复制一份数据。
    // QBuffer::setData() 会在内部保存副本；QPdfDocument::load(QIODevice*) 对
    // 可随机访问的 QBuffer 是同步的（本机 Qt 6.8.3 实测：load 返回即
    // status=Ready、pageCount 可用），无需等待 statusChanged 信号。
    // 注意该重载返回 void，加载结果只能用 error() / pageCount() 判断。
    m_pdfBuf.setData(QByteArray(szBuf, static_cast<qsizetype>(nFileLen)));
    if (!m_pdfBuf.open(QIODevice::ReadOnly))
    {
        qWarning() << "[PluginPdf] 无法打开内存 PDF 缓冲区.";
        showLoadError(QStringLiteral("无法打开 PDF 内存缓冲区。"));
        return false;
    }

    m_doc.load(&m_pdfBuf);
    if (m_doc.error() != QPdfDocument::Error::None || m_doc.pageCount() <= 0)
    {
        qWarning() << "[PluginPdf] PDF 加载失败, code =" << int(m_doc.error())
                   << ", pageCount =" << m_doc.pageCount();
        showLoadError(m_doc.error() != QPdfDocument::Error::None
                          ? pdfErrorText(m_doc.error())
                          : QStringLiteral("未能读出任何页面（PDF 可能为空或已损坏）。"));
        return false;
    }

    m_nPageCount = m_doc.pageCount();
    m_nCurPage = 0;
    gotoPage();
    return true;
}

bool MainWindow::showPdfFile(const QString &sPath)
{
    clearContent();   // 同上：先复位，失败时不会残留旧页面
    if (m_doc.load(sPath) != QPdfDocument::Error::None || m_doc.pageCount() <= 0)
    {
        qWarning() << "[PluginPdf] 按路径加载 PDF 失败:" << sPath << "code =" << int(m_doc.error());
        showLoadError(m_doc.error() != QPdfDocument::Error::None
                          ? pdfErrorText(m_doc.error())
                          : QStringLiteral("未能读出任何页面（PDF 可能为空或已损坏）。"));
        return false;
    }
    m_nPageCount = m_doc.pageCount();
    m_nCurPage = 0;
    gotoPage();
    return true;
}

void MainWindow::on_actionFirstPage_triggered()
{
    if (m_nPageCount <= 0)
        return;
    // P2-10：导航入口一律把页码夹在 [0, pageCount-1]
    m_nCurPage = 0;
    gotoPage();
}

void MainWindow::on_actionPrePage_triggered()
{
    if(m_nPageCount <= 0)
        return;
    m_nCurPage = qBound(0, m_nCurPage - 1, m_nPageCount - 1);
    gotoPage();
}

void MainWindow::on_actionNextPage_triggered()
{
    if(m_nPageCount <= 0)
        return;
    m_nCurPage = qBound(0, m_nCurPage + 1, m_nPageCount - 1);
    gotoPage();
}

void MainWindow::on_actionLastPage_triggered()
{
    if(m_nPageCount <= 0)
        return;
    m_nCurPage = m_nPageCount - 1;
    gotoPage();
}

void MainWindow::on_actionZoomIn_triggered()
{
    // 原判断是 >300，从 300 还能再加到 305%，越过声明的上限；改为 >= 停在 300%
    if(m_nScale >= 300)
    {
        return;
    }
    m_nScale += 5;

    updatePage();
}

void MainWindow::on_actionZoomOut_triggered()
{
    // 同理，原判断 <20 会让 20% 再减到 15%，越过声明的下限
    if(m_nScale <= 20)
    {
        return;
    }
    m_nScale -= 5;

    updatePage();
}

void MainWindow::gotoPage()
{
    getLineEditPageNum();
    // P2-10：页码在统一出口夹紧，任何导航路径（按钮 / 页码输入框 / 刷新）都不会越界
    clampPageIndex();
    updatePageActions();
    setLineEditPageNum();
    updatePage();
}

void MainWindow::clampPageIndex()
{
    if (m_nPageCount > 0)
        m_nCurPage = qBound(0, m_nCurPage, m_nPageCount - 1);
    else
        m_nCurPage = 0;
}

void MainWindow::updatePageActions()
{
    // P2-10：0 页（空文档/加载失败）时四个翻页动作全部禁用
    const bool bHasDoc = (m_nPageCount > 0);
    ui->actionFirstPage->setEnabled(bHasDoc);
    ui->actionPrePage->setEnabled(bHasDoc);
    ui->actionNextPage->setEnabled(bHasDoc);
    ui->actionLastPage->setEnabled(bHasDoc);
    if (m_lePageNum)
        m_lePageNum->setEnabled(bHasDoc);
}

QPixmap MainWindow::makeErrorPlaceholder(const QSize &size, const QString &msg) const
{
    // 目标尺寸过小/无效时退回 A4@72dpi，保证提示文字可读
    QSize sz = size;
    if (sz.width() < 240 || sz.height() < 240)
        sz = QSize(595, 842);
    QPixmap pm(sz);
    pm.fill(QColor(255, 255, 255));
    QPainter painter(&pm);
    painter.setPen(QColor(176, 32, 32));
    QFont font = painter.font();
    font.setPointSizeF(qMax(10.0, qMin(sz.width(), sz.height()) / 30.0));
    painter.setFont(font);
    painter.drawText(pm.rect().adjusted(16, 16, -16, -16),
                     Qt::AlignCenter | Qt::TextWordWrap, msg);
    painter.end();
    return pm;
}

void MainWindow::updatePage()
{
    // P2-10：空文档由 clearContent() 显示"空文档"占位，这里不再渲染
    if (m_nPageCount <= 0)
        return;

    // 页码越界时先夹紧并同步页码框，保证"显示的页码"和"渲染的页"一致
    if (m_nCurPage < 0 || m_nCurPage >= m_nPageCount)
    {
        clampPageIndex();
        setLineEditPageNum();
    }

    // pagePointSize() 返回页面尺寸（单位 point，1/72 inch）。沿用原 QtPdfium
    // 的 72dpi 基准：100% 缩放时 A4 约为 595x842 像素，保持视觉尺寸不变。
    // 直接按目标分辨率调用 render()，比先渲染再缩放更清晰。
    const QSizeF pt = m_doc.pagePointSize(m_nCurPage);
    QSize target;
    if (!pt.isEmpty())
    {
        const qreal factor = qreal(m_nScale) / 100.0;
        target = QSize(qMax(1, qRound(pt.width()  * factor)),
                       qMax(1, qRound(pt.height() * factor)));
        // 433 字节的 PDF 也能声明 12000x12000pt；无上限会一次分配 ~1.1GB。
        constexpr int kMaxPdfRenderDim = 4096;
        if (target.width() > kMaxPdfRenderDim || target.height() > kMaxPdfRenderDim)
            target.scale(kMaxPdfRenderDim, kMaxPdfRenderDim, Qt::KeepAspectRatio);
        const QImage pdfImage = m_doc.render(m_nCurPage, target);
        if (!pdfImage.isNull())
        {
            m_lbContent->setPixmap(QPixmap::fromImage(pdfImage));
            return;
        }
        qWarning() << "[PluginPdf] 第" << (m_nCurPage + 1) << "页渲染失败（返回空图）";
    }
    else
    {
        qWarning() << "[PluginPdf] 第" << (m_nCurPage + 1) << "页尺寸无效，无法渲染";
    }

    // P2-10：渲染失败/尺寸无效时，用"当前页"的错误占位图替换画面，
    // 而不是继续显示上一页的 pixmap（否则页码和画面会对不上）。
    m_lbContent->setPixmap(makeErrorPlaceholder(
        target, tr("第 %1 页渲染失败\n\n文档可能已损坏，或该页内容不受支持。").arg(m_nCurPage + 1)));
}

void MainWindow::getLineEditPageNum()
{
    QString sPageNum = m_lePageNum->text();
    if(sPageNum.isEmpty())
        return;
    int nIndex = sPageNum.indexOf("/");
    if(nIndex != -1)
        return;
    // toInt() 失败会返回 0，直接减 1 会把当前页改成 -1（空文档下显示 0/0）。
    // 只接受合法的正整数页码，其余输入保持当前页不变。
    bool ok = false;
    const int typed = sPageNum.toInt(&ok);
    if (!ok || typed < 1)
        return;
    m_nCurPage = typed - 1;
    // 限制页码到有效范围
    if (m_nPageCount > 0)
    {
        if (m_nCurPage < 0) m_nCurPage = 0;
        if (m_nCurPage >= m_nPageCount) m_nCurPage = m_nPageCount - 1;
    }
}

void MainWindow::setLineEditPageNum()
{
    if (!m_lePageNum)
        return;
    // P2-10：0 页时显示"空文档"，不再出现 "1/0"
    if (m_nPageCount <= 0)
    {
        m_lePageNum->setText(tr("空文档"));
        return;
    }
    QString sLePageNum = QString("%1/%2").arg(QString::number(m_nCurPage + 1)).arg(QString::number(m_nPageCount));
    m_lePageNum->setText(sLePageNum);
}

#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QAction>
#include <QDesktopServices>
#include <QFileInfo>
#include <QLocale>
#include <QScrollBar>
#include <QTextDocument>
#include <QUrl>
#include <QWheelEvent>
#include <QtGlobal>

namespace {
// 缩放范围与步长：0.5x ~ 3.0x，每次 10%
constexpr double kMinScale = 0.5;
constexpr double kMaxScale = 3.0;
constexpr double kZoomStep = 1.1;
// 一次性读进内存显示的上限（docx 常见几 MB，超过这个量级基本是病态文件）
constexpr qint64 kMaxLoadBytes = 512LL * 1024 * 1024;
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    // 文档里可能有超链接：不接管内部导航，只把 http(s)/mailto 交给系统浏览器
    ui->textBrowser->setOpenLinks(false);
    ui->textBrowser->setOpenExternalLinks(false);
    connect(ui->textBrowser, &QTextBrowser::anchorClicked, this, [](const QUrl &url) {
        const QString s = url.scheme().toLower();
        if (s == QLatin1String("http") || s == QLatin1String("https")
            || s == QLatin1String("mailto") || s == QLatin1String("ftp"))
            QDesktopServices::openUrl(url);
    });

    // 图片走 resourceProvider：与 setHtml 的先后顺序无关，也不会被 setHtml 清掉。
    // 这里按值取图，lambda 在文档存活期间（= 本窗口存活期间）一直有效。
    ui->textBrowser->document()->setResourceProvider(
        [this](const QUrl &url) -> QVariant {
            const auto it = m_images.constFind(url.toString());
            return it == m_images.constEnd() ? QVariant() : QVariant(it.value());
        });

    // Ctrl + 滚轮缩放
    ui->textBrowser->viewport()->installEventFilter(this);

    ui->actionZoomIn->setEnabled(false);
    ui->actionZoomOut->setEnabled(false);
    ui->actionZoomReset->setEnabled(false);
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::setSourceName(const QString &sName)
{
    m_sourceName = sName;
}

void MainWindow::reset()
{
    m_doc.reset();
    m_scale = 1.0;
    m_images.clear();
    ui->textBrowser->clear();
    ui->statusBar->clearMessage();

    ui->actionZoomIn->setEnabled(false);
    ui->actionZoomOut->setEnabled(false);
    ui->actionZoomReset->setEnabled(false);

    QString title = QStringLiteral("Word 文档");
    if (!m_sourceName.isEmpty()) title += QStringLiteral(" - ") + m_sourceName;
    setWindowTitle(title);
}

void MainWindow::clearContent()
{
    reset();
    showMessage(QStringLiteral("文件为空或无法读取。"));
}

// 旧通道（char*）：只剩兼容意义 —— 主程序已经改走 sendFile(PluginFile)。
// 这里必然要复制一次，因为裸指针不携带所有权信息。
void MainWindow::showWord(char *szBuf, qint64 nFileLen)
{
    if (szBuf == nullptr || nFileLen <= 0)
        showWord(QByteArray());
    else
        showWord(QByteArray(szBuf, static_cast<qsizetype>(nFileLen)));
}

// C2：新的 QByteArray 通道 —— sendFile(PluginFile) 直接把数据交过来，
// 这里不再复制（QByteArray 隐式共享，WordDocument/ZipReader 持引用即可）。
void MainWindow::showWord(const QByteArray &data)
{
    reset();

    if (data.isEmpty())
    {
        showMessage(QStringLiteral("文件为空或无法读取。"));
        return;
    }
    if (data.size() > kMaxLoadBytes)
    {
        showMessage(QStringLiteral("文件过大（%1），无法一次性载入内存显示。")
                        .arg(QLocale().formattedDataSize(data.size())));
        return;
    }

    // 每个文件用独立的图片 URL 前缀：同一个 QTextDocument 里上一份文档
    // 缓存的图片资源不会与新文档的 URL 撞车。
    m_doc.setUrlPrefix(QStringLiteral("wimg://doc%1/").arg(++m_generation));

    QString err;
    if (!m_doc.open(data, &err))
    {
        showMessage(err.isEmpty()
                    ? QStringLiteral("无法解析该 Word 文档。") : err);
        return;
    }

    m_scale = 1.0;

    ui->actionZoomIn->setEnabled(true);
    ui->actionZoomOut->setEnabled(true);
    ui->actionZoomReset->setEnabled(true);

    renderDocument();

    const WordDocument::Stats &now = m_doc.stats();
    if (now.paragraphs == 0 && now.tables == 0)
    {
        showMessage(QStringLiteral("这个文档里没有可显示的正文内容"
                                   "（可能只有页眉页脚、文本框或图片之外的嵌入对象）。"));
        return;
    }
}

void MainWindow::renderDocument()
{
    if (!m_doc.isOpen()) return;

    QScrollBar *bar = ui->textBrowser->verticalScrollBar();
    const int oldMax = bar->maximum();
    const double ratio = oldMax > 0 ? double(bar->value()) / double(oldMax) : 0.0;

    WordDocument::RenderResult res = m_doc.render(m_scale);
    m_images = res.images;

    ui->textBrowser->setHtml(res.html);

    // 恢复滚动位置（缩放后仍停在原来大致的位置）
    bar->setValue(int(ratio * bar->maximum()));

    updateStatus();
}

void MainWindow::updateStatus()
{
    if (!m_doc.isOpen())
    {
        ui->statusBar->clearMessage();
        return;
    }

    const WordDocument::Stats &s = m_doc.stats();
    QStringList parts;
    parts << QStringLiteral("缩放 %1%").arg(int(m_scale * 100 + 0.5));
    parts << QStringLiteral("%1 段").arg(s.paragraphs);
    if (s.headings > 0) parts << QStringLiteral("标题 %1").arg(s.headings);
    if (s.tables > 0)   parts << QStringLiteral("表格 %1").arg(s.tables);
    if (s.images > 0)   parts << QStringLiteral("图片 %1").arg(s.images);
    parts << QStringLiteral("约 %1 字").arg(s.chars);
    ui->statusBar->showMessage(parts.join(QStringLiteral(" · ")));
}

void MainWindow::showMessage(const QString &text)
{
    QString body = text;
    body.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    body.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    body.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    body.replace(QLatin1String("\n"), QLatin1String("<br/>"));

    ui->textBrowser->setHtml(QStringLiteral(
        "<div style=\"margin:24px;font-size:11pt;line-height:150%\">%1</div>").arg(body));
    ui->statusBar->clearMessage();
}

void MainWindow::applyZoom(double scale)
{
    if (!m_doc.isOpen()) return;

    const double clamped = qBound(kMinScale, scale, kMaxScale);
    if (qAbs(clamped - m_scale) < 0.0001) return;

    m_scale = clamped;
    renderDocument();
}

void MainWindow::on_actionZoomIn_triggered()
{
    applyZoom(m_scale * kZoomStep);
}

void MainWindow::on_actionZoomOut_triggered()
{
    applyZoom(m_scale / kZoomStep);
}

void MainWindow::on_actionZoomReset_triggered()
{
    applyZoom(1.0);
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == ui->textBrowser->viewport() && event->type() == QEvent::Wheel)
    {
        QWheelEvent *we = static_cast<QWheelEvent *>(event);
        if (we->modifiers() & Qt::ControlModifier)
        {
            const int dy = we->angleDelta().y();
            if (dy != 0)
            {
                applyZoom(dy > 0 ? m_scale * kZoomStep : m_scale / kZoomStep);
                return true;      // 吃掉事件，避免同时滚动
            }
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

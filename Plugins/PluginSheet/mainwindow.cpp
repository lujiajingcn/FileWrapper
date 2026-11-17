#include "mainwindow.h"

#include <QTextBrowser>
#include <QVBoxLayout>
#include <QToolBar>
#include <QAction>
#include <algorithm>

MainWindow::MainWindow(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto *bar = new QToolBar(this);
    QAction *pIn = bar->addAction(tr("放大"));
    QAction *pOut = bar->addAction(tr("缩小"));
    QAction *pReset = bar->addAction(tr("重置"));
    layout->addWidget(bar);
    connect(pIn, &QAction::triggered, this, &MainWindow::zoomIn);
    connect(pOut, &QAction::triggered, this, &MainWindow::zoomOut);
    connect(pReset, &QAction::triggered, this, &MainWindow::zoomReset);

    m_browser = new QTextBrowser(this);
    m_browser->setOpenLinks(false);
    m_browser->setOpenExternalLinks(false);
    layout->addWidget(m_browser);
}

MainWindow::~MainWindow() = default;

void MainWindow::rerender()
{
    if (m_hasDoc)
        m_browser->setHtml(m_doc.render(m_fontScale));
}

void MainWindow::zoomIn()
{
    m_fontScale = std::min(3.0, m_fontScale + 0.15);
    rerender();
}

void MainWindow::zoomOut()
{
    m_fontScale = std::max(0.5, m_fontScale - 0.15);
    rerender();
}

void MainWindow::zoomReset()
{
    m_fontScale = 1.0;
    rerender();
}

void MainWindow::clearContent()
{
    m_hasDoc = false;
    m_fontScale = 1.0;
    m_doc.reset();
    m_browser->clear();
}

void MainWindow::showSheet(const QByteArray &data)
{
    clearContent();

    QString err;
    if (!m_doc.open(data, &err))
    {
        showMessage(err.isEmpty() ? QStringLiteral("无法解析该表格文件。") : err);
        return;
    }
    m_hasDoc = true;
    m_browser->setHtml(m_doc.render(m_fontScale));
}

void MainWindow::showMessage(const QString &text)
{
    m_hasDoc = false;
    QString body = text;
    body.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    body.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    body.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    body.replace(QLatin1String("\n"), QLatin1String("<br/>"));

    m_browser->setHtml(QStringLiteral(
        "<div style=\"margin:24px;font-size:11pt;line-height:150%\">%1</div>").arg(body));
}

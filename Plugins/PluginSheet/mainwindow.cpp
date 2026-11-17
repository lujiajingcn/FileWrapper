#include "mainwindow.h"

#include <QTextBrowser>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_browser = new QTextBrowser(this);
    m_browser->setOpenLinks(false);
    m_browser->setOpenExternalLinks(false);
    layout->addWidget(m_browser);
}

MainWindow::~MainWindow() = default;

void MainWindow::clearContent()
{
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
    m_browser->setHtml(m_doc.render(1.0));
}

void MainWindow::showMessage(const QString &text)
{
    QString body = text;
    body.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    body.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    body.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    body.replace(QLatin1String("\n"), QLatin1String("<br/>"));

    m_browser->setHtml(QStringLiteral(
        "<div style=\"margin:24px;font-size:11pt;line-height:150%\">%1</div>").arg(body));
}

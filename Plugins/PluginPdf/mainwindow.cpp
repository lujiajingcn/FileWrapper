#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QDebug>
#include <QImage>
#include <QPixmap>

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
}

MainWindow::~MainWindow()
{
    delete m_lePageNum;
    delete ui;
}

void MainWindow::showPdf(char *szBuf, qint64 nFileLen)
{
    if (szBuf == nullptr || nFileLen <= 0)
        return;

    // 主程序会在 sendFileData 返回后释放原始缓冲区，因此必须复制一份数据。
    // QBuffer::setData() 会在内部保存副本；QPdfDocument::load(QIODevice*) 对
    // 可随机访问的 QBuffer 是同步的（Qt 6.9 实测：load 返回即 status=Ready、
    // pageCount 可用），无需等待 statusChanged 信号。
    m_doc.close();
    m_pdfBuf.close();
    m_pdfBuf.setData(QByteArray(szBuf, static_cast<qsizetype>(nFileLen)));
    if (!m_pdfBuf.open(QIODevice::ReadOnly))
    {
        qWarning() << "[PluginPdf] 无法打开内存 PDF 缓冲区.";
        return;
    }

    m_doc.load(&m_pdfBuf);
    if (m_doc.error() != QPdfDocument::Error::None)
        qWarning() << "[PluginPdf] PDF 加载错误, code =" << int(m_doc.error());

    m_nPageCount = m_doc.pageCount();
    if (m_nPageCount < 0) m_nPageCount = 0;
    m_nCurPage = 0;
    gotoPage();
}

bool MainWindow::showPdfFile(const QString &sPath)
{
    m_doc.close();
    m_pdfBuf.close();
    m_pdfBuf.setData(QByteArray());   // 释放内存副本
    if (m_doc.load(sPath) != QPdfDocument::Error::None)
    {
        qWarning() << "[PluginPdf] 按路径加载 PDF 失败:" << sPath << "code =" << int(m_doc.error());
        return false;
    }
    m_nPageCount = qMax(0, m_doc.pageCount());
    m_nCurPage = 0;
    gotoPage();
    return true;
}

void MainWindow::on_actionFirstPage_triggered()
{
    m_nCurPage = 0;
    gotoPage();
}

void MainWindow::on_actionPrePage_triggered()
{
    if(m_nCurPage > 0)
    {
        m_nCurPage--;
        gotoPage();
    }
}

void MainWindow::on_actionNextPage_triggered()
{
    if(m_nPageCount <= 0 || m_nCurPage >= m_nPageCount - 1)
        return;
    m_nCurPage++;
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
    if(m_nScale > 300)
    {
        return;
    }
    m_nScale += 5;

    updatePage();
}

void MainWindow::on_actionZoomOut_triggered()
{
    if(m_nScale < 20)
    {
        return;
    }
    m_nScale -= 5;

    updatePage();
}

void MainWindow::gotoPage()
{
    getLineEditPageNum();
    setLineEditPageNum();
    updatePage();
}

void MainWindow::updatePage()
{
    if (m_nPageCount <= 0 || m_nCurPage < 0 || m_nCurPage >= m_nPageCount)
        return;

    // pagePointSize() 返回页面尺寸（单位 point，1/72 inch）。沿用原 QtPdfium
    // 的 72dpi 基准：100% 缩放时 A4 约为 595x842 像素，保持视觉尺寸不变。
    // 直接按目标分辨率调用 render()，比先渲染再缩放更清晰。
    const QSizeF pt = m_doc.pagePointSize(m_nCurPage);
    if (pt.isEmpty())
        return;

    const qreal factor = qreal(m_nScale) / 100.0;
    const QSize target(qMax(1, qRound(pt.width()  * factor)),
                       qMax(1, qRound(pt.height() * factor)));
    const QImage pdfImage = m_doc.render(m_nCurPage, target);
    if (pdfImage.isNull())
        return;

    m_lbContent->setPixmap(QPixmap::fromImage(pdfImage));
}

void MainWindow::getLineEditPageNum()
{
    QString sPageNum = m_lePageNum->text();
    if(sPageNum.isEmpty())
        return;
    int nIndex = sPageNum.indexOf("/");
    if(nIndex != -1)
        return;
    m_nCurPage = sPageNum.toInt() - 1;
    // 限制页码到有效范围
    if (m_nPageCount > 0)
    {
        if (m_nCurPage < 0) m_nCurPage = 0;
        if (m_nCurPage >= m_nPageCount) m_nCurPage = m_nPageCount - 1;
    }
}

void MainWindow::setLineEditPageNum()
{
    QString sLePageNum = QString("%1/%2").arg(QString::number(m_nCurPage + 1)).arg(QString::number(m_nPageCount));
    m_lePageNum->setText(sLePageNum);
}

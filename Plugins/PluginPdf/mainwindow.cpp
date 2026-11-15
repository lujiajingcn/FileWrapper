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

void MainWindow::clearContent()
{
    // 成员声明顺序保证 m_pdfBuf 在 m_doc.close() 时仍然有效
    m_doc.close();
    m_pdfBuf.close();
    m_pdfBuf.setData(QByteArray());
    m_nPageCount = 0;
    m_nCurPage = 0;
    if (m_lbContent) m_lbContent->clear();
    if (m_lePageNum) m_lePageNum->clear();
}

void MainWindow::showPdf(char *szBuf, qint64 nFileLen)
{
    // 换文件先清掉上一份的画面/页码/文档：任何失败路径都不会残留旧 PDF 页面。
    // 原实现只在 load 失败时 qWarning，而 updatePage() 在 m_nPageCount<=0 时
    // 直接 return，导致界面仍显示上一个文件的内容。
    clearContent();

    if (szBuf == nullptr || nFileLen <= 0)
        return;

    // 主程序会在 sendFileData 返回后释放原始缓冲区，因此必须复制一份数据。
    // QBuffer::setData() 会在内部保存副本；QPdfDocument::load(QIODevice*) 对
    // 可随机访问的 QBuffer 是同步的（本机 Qt 6.8.3 实测：load 返回即
    // status=Ready、pageCount 可用），无需等待 statusChanged 信号。
    // 注意该重载返回 void，加载结果只能用 error() / pageCount() 判断。
    m_pdfBuf.setData(QByteArray(szBuf, static_cast<qsizetype>(nFileLen)));
    if (!m_pdfBuf.open(QIODevice::ReadOnly))
    {
        qWarning() << "[PluginPdf] 无法打开内存 PDF 缓冲区.";
        return;
    }

    m_doc.load(&m_pdfBuf);
    if (m_doc.error() != QPdfDocument::Error::None || m_doc.pageCount() <= 0)
    {
        qWarning() << "[PluginPdf] PDF 加载失败, code =" << int(m_doc.error())
                   << ", pageCount =" << m_doc.pageCount();
        return;
    }

    m_nPageCount = m_doc.pageCount();
    m_nCurPage = 0;
    gotoPage();
}

bool MainWindow::showPdfFile(const QString &sPath)
{
    clearContent();   // 同上：先复位，失败时不会残留旧页面
    if (m_doc.load(sPath) != QPdfDocument::Error::None || m_doc.pageCount() <= 0)
    {
        qWarning() << "[PluginPdf] 按路径加载 PDF 失败:" << sPath << "code =" << int(m_doc.error());
        return false;
    }
    m_nPageCount = m_doc.pageCount();
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
    QString sLePageNum = QString("%1/%2").arg(QString::number(m_nCurPage + 1)).arg(QString::number(m_nPageCount));
    m_lePageNum->setText(sLePageNum);
}

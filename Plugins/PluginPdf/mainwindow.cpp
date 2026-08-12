#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QDebug>
#include <QFile>

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
    delete ui;
}

void MainWindow::showPdf(char *szBuf, qint64 nFileLen)
{
    // 复制一份数据由本对象持有：PDFium 的 loadFromMem 只保存数据指针而不复制，
    // 主程序会在 sendFileData 返回后释放原始缓冲区，
    // 若不复制，翻页时访问已释放内存会导致 use-after-free 崩溃。
    m_pdfData = QByteArray(szBuf, nFileLen);
    m_pdfium.loadFromMem(m_pdfData.constData(), m_pdfData.size());
    m_nPageCount = m_pdfium.pageCount();
    gotoPage();
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

//    用以下写法得到的QPixmap变量值为空。
//    QPixmap pixMap;
//    pixMap.fromImage(pdfImage);
//    qDebug()<<pixMap.isNull();
}

void MainWindow::updatePage()
{
    if(!m_pdfium.isValid() || m_nPageCount <= 0 || m_nCurPage < 0 || m_nCurPage >= m_nPageCount)
        return;

    QPdfiumPage pfPage = m_pdfium.page(m_nCurPage);
    int nPageWidth = pfPage.width();
    int nPageHeight = pfPage.height();
    QImage pdfImage = pfPage.image();
    if(pdfImage.isNull())
        return;
    QPixmap pixMap = QPixmap::fromImage(pdfImage);
    int nNewWidth = nPageWidth * m_nScale / 100;
    int nNewHeight = nPageHeight * m_nScale / 100;
    pixMap = pixMap.scaledToWidth(nNewWidth);
    pixMap = pixMap.scaledToHeight(nNewHeight);
    m_lbContent->setPixmap(pixMap);
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
}

void MainWindow::setLineEditPageNum()
{
    QString sLePageNum = QString("%1/%2").arg(QString::number(m_nCurPage + 1)).arg(QString::number(m_nPageCount));
    m_lePageNum->setText(sLePageNum);
}

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QByteArray>
#include <QBuffer>
#include <QtPdf/QPdfDocument>
#include <QLabel>
#include <QLineEdit>

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    void showPdf(char *szBuf, qint64 nFileLen);
    // 大文件按路径加载，QPdfDocument 自行按需读取，避免全量内存副本（P1-2）
    bool showPdfFile(const QString &sPath);

private slots:
    void on_actionFirstPage_triggered();

    void on_actionPrePage_triggered();

    void on_actionNextPage_triggered();

    void on_actionLastPage_triggered();

    void on_actionZoomIn_triggered();

    void on_actionZoomOut_triggered();

private slots:
    void gotoPage();

private:
    void updatePage();
    void getLineEditPageNum();
    void setLineEditPageNum();

private:
    // 声明顺序重要：QBuffer 在 QPdfDocument 之前构造、之后析构，
    // 保证 m_doc.close() 时其 QIODevice 仍然有效。
    QBuffer        m_pdfBuf;    // 持有 PDF 数据副本，避免主程序释放原缓冲后 use-after-free
    QPdfDocument   m_doc;
    int            m_nPageCount;
    int            m_nCurPage;
    int            m_nScale;
    QLabel        *m_lbContent;
    QLineEdit     *m_lePageNum;

    Ui::MainWindow *ui;
};

#endif // MAINWINDOW_H

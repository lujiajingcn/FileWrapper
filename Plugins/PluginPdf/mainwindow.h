#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QByteArray>
#include "qpdfium.h"
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

    QPdfium         m_pdfium;
    int             m_nPageCount;
    int             m_nCurPage;
    int             m_nScale;
    QLabel          *m_lbContent;
    QLineEdit       *m_lePageNum;
    QByteArray      m_pdfData;  // 持有 PDF 数据副本，防止主程序 delete[] 后 use-after-free

    Ui::MainWindow *ui;
};

#endif // MAINWINDOW_H

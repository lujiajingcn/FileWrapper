#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QWidget>
#include <QByteArray>
#include <QString>

#include "sheetdocument.h"

class QTextBrowser;

// 只读表格查看器：把 xlsx/ods 的首层数据渲染成 HTML 表格。
class MainWindow : public QWidget
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void showSheet(const QByteArray &data);
    void clearContent();
    void setSourceName(const QString &name) { m_sourceName = name; }

private:
    void showMessage(const QString &text);

    SheetDocument  m_doc;
    QTextBrowser  *m_browser = nullptr;
    QString        m_sourceName;
};

#endif // MAINWINDOW_H

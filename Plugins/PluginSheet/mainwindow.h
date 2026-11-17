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

    // 缩放（渲染层已支持 fontScale，这里提供 UI 入口）
    void zoomIn();
    void zoomOut();
    void zoomReset();

private:
    void showMessage(const QString &text);
    void rerender();

    SheetDocument  m_doc;
    QTextBrowser  *m_browser = nullptr;
    QString        m_sourceName;
    double         m_fontScale = 1.0;
    bool           m_hasDoc = false;
};

#endif // MAINWINDOW_H

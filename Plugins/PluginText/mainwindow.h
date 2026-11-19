#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QString>
#include <QByteArray>

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    void showText(char *szBuf, qint64 nFileLen);
    // 打开失败 / 0 字节时清空内容区，避免界面残留上一个文件
    void clearContent();

    // 主程序在送数据前告知文件名：.html/.htm 用富文本渲染，其它仍按纯文本显示
    void setSourceName(const QString &name) { m_sourceName = name; }

    // 在当前文本中查找（Ctrl+F / F3 / 工具栏），找不到时自动从头/尾循环一次
    bool findText(const QString &sText, bool bForward = true);

private:
    // 按编码（BOM -> 严格 UTF-8 -> 系统本地编码 GBK）解码文本，避免 .sql 等文件乱码。
    // pbTruncatedTail（可空）：末尾多字节序列不完整、只解码了有效前缀时置 true（P2-2）。
    static QString decodeText(const QByteArray &data, bool *pbTruncatedTail = nullptr);
    // 文件名未知时，用内容嗅探判断是不是 HTML
    static bool looksLikeHtml(const QString &text);

    Ui::MainWindow *ui;
    QString m_sourceName;
};

#endif // MAINWINDOW_H

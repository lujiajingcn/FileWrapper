#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QMap>
#include <QString>

#include "worddocument.h"

namespace Ui {
class MainWindow;
}

// Word 文档显示窗口：把 docx 转换成 HTML 后放进 QTextBrowser 显示。
// 每个标签页由 PluginWord::createInstance() 建一个独立实例，互不影响。
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    // 主程序把整个 .docx 的字节交进来；返回后即可释放该缓冲
    void showWord(char *szBuf, qint64 nFileLen);
    // C2：sendFile(PluginFile) 通道，直接接收 QByteArray（不再复制一份）
    void showWord(const QByteArray &data);
    // 主程序在送数据前告知文件名（用作窗口标题回退）
    void setSourceName(const QString &sName);
    // 打开失败 / 0 字节时清空内容区，避免界面残留上一个文件
    void clearContent();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private slots:
    // 由 ui 的 connectSlotsByName 自动连接（命名必须保持 on_<对象名>_<信号>）
    void on_actionZoomIn_triggered();
    void on_actionZoomOut_triggered();
    void on_actionZoomReset_triggered();

private:
    void reset();
    void renderDocument();            // 按当前 m_scale 重新生成并显示 HTML
    void applyZoom(double scale);
    void showMessage(const QString &text);
    void updateStatus();

    Ui::MainWindow *ui;
    WordDocument    m_doc;
    QString         m_sourceName;
    // 当前文档的图片资源（URL -> 图像）。文档的 resourceProvider 从这里取图，
    // 于是不必关心 setHtml 与 addResource 的先后顺序。
    QMap<QString, QImage> m_images;
    double          m_scale = 1.0;
    // 正文为空（0 段 0 表）：此时界面显示的是提示文字，缩放动作禁用、
    // renderDocument() 不再 setHtml 空内容覆盖提示（P-11）
    bool            m_emptyDocument = false;
    quint64         m_generation = 0;   // 每打开一个文件 +1，用于生成不复用的图片 URL
};

#endif // MAINWINDOW_H

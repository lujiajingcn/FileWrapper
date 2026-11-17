#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QWidget>
#include <QByteArray>
#include <QImage>
#include <QMap>
#include <QString>

#include "slidesdocument.h"

class QTextBrowser;

// 只读演示查看器：逐页渲染 pptx/odp 的标题、正文与图片。
class MainWindow : public QWidget
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void showSlides(const QByteArray &data);
    void clearContent();
    void setSourceName(const QString &name) { m_sourceName = name; }

    // 缩放（渲染层已支持 fontScale，这里提供 UI 入口）
    void zoomIn();
    void zoomOut();
    void zoomReset();

private:
    void showMessage(const QString &text);
    void rerender();

    SlidesDocument m_doc;
    QTextBrowser  *m_browser = nullptr;
    QMap<QString, QImage> m_images;   // 当前文档的图片资源（resourceProvider 用）
    QString        m_sourceName;
    double         m_fontScale = 1.0;
    bool           m_hasDoc = false;
};

#endif // MAINWINDOW_H

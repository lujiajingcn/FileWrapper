#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    void showPicture(char *szBuf, qint64 nFileLen);
    // 打开失败 / 0 字节时清空内容区，避免界面残留上一个文件
    void clearContent();

    // 视图动作（工具栏按钮与探针共用；不是 .ui 里的 action，不需要 on_ 命名）
    void fitToWindow();     // 适应窗口
    void actualSize();      // 实际大小 100%

private slots:
    void on_actZoomIn_triggered();
    void on_actZoomOut_triggered();
    void on_actRotate_triggered();

private:
    void applyTransform();  // 根据当前缩放/旋转重新绘制
    void showError(const QString &msg);          // 清图 + 提示文案（顺序固定，P1-22）
    QSize previewViewportSize() const;           // 初始预览可用的视口尺寸（P1-22）

private:
    Ui::MainWindow *ui;

    QPixmap  m_originalPix;   // 原始图片（>40 MP 的大图可能是按视口缩小的预览解码结果）
    qreal    m_scale = 1.0;   // 当前缩放比例（已包含像素上限修正后的实际值，P2-13）
    int      m_rotateDeg = 0; // 当前旋转角度 (0/90/180/270)
    bool     m_bPreviewScaled = false; // P1-22：解码时已用 setScaledSize() 缩到视口
    QSize    m_declaredSize;           // P1-22：解码前读到的原始声明尺寸（状态栏提示用）
};

#endif // MAINWINDOW_H

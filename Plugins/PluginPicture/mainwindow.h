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

private slots:
    void on_actZoomIn_triggered();
    void on_actZoomOut_triggered();
    void on_actRotate_triggered();

private:
    void applyTransform();  // 根据当前缩放/旋转重新绘制

private:
    Ui::MainWindow *ui;

    QPixmap  m_originalPix;   // 原始图片 (不变)
    qreal    m_scale = 1.0;   // 当前缩放比例
    int      m_rotateDeg = 0; // 当前旋转角度 (0/90/180/270)
};

#endif // MAINWINDOW_H

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

private:
    // 按编码（BOM -> UTF-8 -> 系统本地编码 GBK）解码文本，避免 .sql 等文件乱码
    static QString decodeText(const QByteArray &data);

    Ui::MainWindow *ui;
};

#endif // MAINWINDOW_H

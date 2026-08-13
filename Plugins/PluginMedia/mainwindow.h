#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QDebug>
#include <QTimer>
#include <QTime>
#include <QAudioOutput>

extern "C"
{
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libswscale/swscale.h>
    #include <libavdevice/avdevice.h>
    #include <libavformat/version.h>
    #include <libavutil/time.h>
    #include <libavutil/mathematics.h>
    #include <libavutil/avutil.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/pixfmt.h>
    #include <libavutil/frame.h>
    #include <libswresample/swresample.h>
}

#define MAX_AUDIO_FRAME_SIZE 192000

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

public slots:
   void timeCallback(void);
   void resizeEvent(QResizeEvent* );

public slots:
   int showVideo(char *szFileData, qint64 nFileLen);

private:
    void clearFFmpegResources();

private:
    Ui::MainWindow *ui;
    QTimer *timer;
    int videoW, videoH;

    AVFormatContext *pFormatCtx = nullptr;
    AVCodecContext  *pCodecCtx  = nullptr;
    AVFrame         *pFrame     = nullptr, *pFrameRGB = nullptr;
    int videoindex = -1;

    int             audioindex = -1;
    AVCodecContext  *aCodecCtx  = nullptr;
    QByteArray      byteBuf;
    QAudioOutput    *audioOutput;
    QIODevice       *streamOut;
};

#endif // MAINWINDOW_H

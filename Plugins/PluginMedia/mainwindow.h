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
    #include <libavfilter/buffersink.h>
    #include <libavfilter/buffersrc.h>
    #include <libavutil/avutil.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/pixfmt.h>
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
   int showVideo(char *szFileData, int nFileLen);

private:
    Ui::MainWindow *ui;
    QTimer *timer;      // 定时播放，根据帧率来
    int videoW,videoH;  // 图像宽高

    AVFormatContext	*pFormatCtx;
    AVCodecContext  *pCodecCtx;
    AVCodec         *pCodec;
    AVFrame         *pFrame, *pFrameRGB;
    int ret, got_picture,got_audio;  // 视频解码标志
    int videoindex;        // 视频序号
    // 音频
    int audioindex;        // 音频序号
    AVCodecParameters   *aCodecParameters;
    AVCodec             *aCodec;
    AVCodecContext      *aCodecCtx;
    QByteArray          byteBuf;//音频缓冲
    QAudioOutput        *audioOutput;
    QIODevice           *streamOut;
};

#endif // MAINWINDOW_H


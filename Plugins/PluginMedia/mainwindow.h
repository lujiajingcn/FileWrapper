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

private slots:
   void on_progressSlider_sliderPressed(void);
   void on_progressSlider_sliderMoved(int value);
   void on_progressSlider_sliderReleased(void);
   void on_playPauseBtn_clicked(void);
   void on_stopBtn_clicked(void);
   void on_volumeSlider_valueChanged(int value);

private:
    void clearFFmpegResources();
    bool decodeOneFrame(AVPacket *packet);
    void doSeek(qint64 ms);
    void updateProgressDisplay();
    void updatePlayPauseIcon();

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

    // ===== 逐帧播放 / 进度条 / seek 相关 =====
    SwrContext        *swr_ctx = nullptr;
    uint8_t           *audio_out_buffer = nullptr;
    struct SwsContext *img_convert_ctx = nullptr;
    AVPacket          *packet = nullptr;
    uint8_t           *out_buffer = nullptr;
    AVChannelLayout    out_ch_layout;

    int    frameRate = 1000 / 25;   // 每帧间隔(ms)
    int    out_sample_rate = 44100;
    bool   m_bPlaying = false;
    bool   m_bPaused = false;
    bool   m_bUserDragging = false;
    bool   m_bSeekPending = false;
    qint64 m_nTargetMs = 0;
    qint64 m_nDurationMs = 0;
    qint64 m_nCurrentMs = 0;
    int    m_nVolume = 80;
};

#endif // MAINWINDOW_H

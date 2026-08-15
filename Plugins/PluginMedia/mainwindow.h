#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QDebug>
#include <QTimer>
#include <QTime>
#include <QAudioOutput>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QAtomicInt>
#include <atomic>

extern "C"
{
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libswscale/swscale.h>
    #include <libavformat/version.h>
    #include <libavutil/time.h>
    #include <libavutil/mathematics.h>
    #include <libavutil/avutil.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/pixfmt.h>
    #include <libavutil/frame.h>
    #include <libavutil/channel_layout.h>
    #include <libswresample/swresample.h>
}

// 音频输出通道数、采样率与采样格式（固定 stereo / S16 / 44100，与 QAudioOutput 配置一致）
static const int           AUDIO_OUT_CHANNELS    = 2;
static const int           AUDIO_OUT_SAMPLE_RATE = 44100;
static const AVSampleFormat AUDIO_OUT_FORMAT     = AV_SAMPLE_FMT_S16;

// 音频缓冲上界：约 2 秒 PCM 数据（防止解码快于播放时无限增长 / 延迟累积）
// ASF/WMV 容器的音频包交错通常不如 MP4 均匀，需要更大的缓冲来吸收突发音频包，
// 避免缓冲区满后丢弃音频块导致"跳帧"。
// 2 通道 * 2 字节(S16) * 44100 * 2
static const int AUDIO_BUFFER_LIMIT_BYTES = AUDIO_OUT_CHANNELS * 2 * AUDIO_OUT_SAMPLE_RATE * 8;

namespace Ui {
class MainWindow;
}

// ===== 视频解码 Worker（运行在独立线程，负责所有解码 + 帧调度） =====
class VideoWorker : public QObject
{
    Q_OBJECT

public:
    explicit VideoWorker(QObject *parent = nullptr);

    // 设置解码所需的 FFmpeg 上下文（主线程调用，doDecode 启动前）
    void setContexts(AVFormatContext *fmtCtx,
                     AVCodecContext *vCodecCtx, int videoIdx,
                     AVCodecContext *aCodecCtx, int audioIdx,
                     int width, int height,
                     int outWidth, int outHeight,
                     SwsContext *swsCtx, AVFrame *frame, AVFrame *frameRGB, AVFrame *audioFrame,
                     uint8_t *outBuf,
                     SwrContext *swr, uint8_t *aOutBuf,
                     QMutex *aMutex, QByteArray *aByteBuf,
                     int outSampleRate, int genId,
                     int audioOutBufCapacity);

    // 阻塞等待 doDecode 退出（用于停止/seek/释放资源），超时返回 false
    bool waitForDecodingStopped(unsigned long timeoutMs = 2000);

signals:
    void frameDecoded(QImage img, qint64 ptsMs, int genId);
    void decodingFinished(int genId);

public slots:
    void doDecode();
    void requestStop();
    void clearStopRequest();
    // 暂停/恢复解码线程的 PTS 帧调度（不退出循环，仅阻塞在条件变量上）
    void setPaused(bool paused);
    // 更新当前播放世代（用于 seek 时过滤排队的旧帧信号）
    void updateGenId(int genId);

    bool isDecodingActive() const { return m_running.load(std::memory_order_relaxed) != 0; }

private:
    AVFormatContext *pFormatCtx = nullptr;
    AVCodecContext  *pCodecCtx  = nullptr;  // video
    int videoindex = -1;

    AVCodecContext  *aCodecCtx  = nullptr;  // audio
    int audioindex = -1;

    int srcW = 0, srcH = 0;
    int outW = 0, outH = 0;
    SwsContext      *imgCtx = nullptr;
    AVFrame         *pFrame = nullptr, *pFrameRGB = nullptr;
    uint8_t         *out_buffer = nullptr;

    SwrContext      *swr_ctx = nullptr;
    AVFrame         *pAudioFrame = nullptr;
    uint8_t         *audio_out_buffer = nullptr;
    QMutex          *m_audioMutex = nullptr;
    QByteArray      *m_audioByteBuf = nullptr;
    int m_outSampleRate = AUDIO_OUT_SAMPLE_RATE;
    int m_audioOutBufCapacity = 0;  // 音频输出缓冲区实际容量（样本数），用作 swr_convert 的 out_count

    int m_genId = -1;

    std::atomic<int> m_stopRequested{0};
    std::atomic<int> m_running{0};
    std::atomic<int> m_paused{0};      // 暂停标志：1 表示暂停帧调度

    qint64 m_lastPtsMs = 0;
    QMutex m_mutex;                    // 保护 setContexts 期间成员的写入，并配合 m_pauseCond/m_stoppedCond
    QWaitCondition m_pauseCond;        // 暂停时阻塞等待，可被唤醒以响应 stop
    QWaitCondition m_stoppedCond;      // doDecode 退出时唤醒等待者
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

public slots:
   void timeCallback(void);

protected:
   void resizeEvent(QResizeEvent*) override;

public slots:
   int showVideo(char *szFileData, qint64 nFileLen);

private slots:
   void on_progressSlider_sliderPressed(void);
   void on_progressSlider_sliderMoved(int value);
   void on_progressSlider_sliderReleased(void);
   void on_playPauseBtn_clicked(void);
   void on_stopBtn_clicked(void);
   void on_volumeSlider_valueChanged(int value);

   // VideoWorker 信号槽
   void onFrameDecoded(QImage img, qint64 ptsMs, int genId);
   void onDecodingFinished(int genId);

private:
    void clearFFmpegResources();
    void doSeek(qint64 ms);
    void updateProgressDisplay();
    void updatePlayPauseIcon();
    // 停止 worker 并阻塞等待其退出 doDecode（替换旧的 busy-wait）
    void stopWorkerAndWait(unsigned long timeoutMs = 2000);
    // 根据源宽高与目标区域计算保持宽高比的目标输出尺寸
    QSize computeAspectRatioSize(int srcW, int srcH, int dstW, int dstH) const;
    // （重新）创建 sws 上下文，将源帧缩放为 outW x outH 的 RGB32
    void rebuildSwsContext(int srcW, int srcH, AVPixelFormat srcPixFmt,
                           int outW, int outH);

private:
    Ui::MainWindow *ui;
    QTimer *timer;
    int videoW = 0, videoH = 0;

    AVFormatContext *pFormatCtx = nullptr;
    AVCodecContext  *pCodecCtx  = nullptr;
    AVFrame         *pFrame     = nullptr, *pFrameRGB = nullptr;
    AVFrame         *pAudioFrame = nullptr;
    unsigned char   *m_avioBuffer = nullptr;
    AVIOContext     *m_avioCtx   = nullptr;
    int videoindex = -1;

    int             audioindex = -1;
    AVCodecContext  *aCodecCtx  = nullptr;
    QByteArray      byteBuf;
    QAudioOutput    *audioOutput = nullptr;
    QIODevice       *streamOut = nullptr;

    // ===== 音频重采样 =====
    SwrContext        *swr_ctx = nullptr;
    uint8_t           *audio_out_buffer = nullptr;
    struct SwsContext *img_convert_ctx = nullptr;
    uint8_t           *out_buffer = nullptr;
    int                out_buffer_size = 0;  // 当前视频输出缓冲大小（字节）
    AVChannelLayout    out_ch_layout;

    // 音频缓存互斥锁
    QMutex m_audioMutex;

    int    out_sample_rate = AUDIO_OUT_SAMPLE_RATE;
    bool   m_bPlaying = false;
    bool   m_bPaused = false;
    bool   m_bUserDragging = false;
    bool   m_bSeekPending = false;
    qint64 m_nTargetMs = 0;
    qint64 m_nDurationMs = 0;
    qint64 m_nCurrentMs = 0;
    int    m_nVolume = 80;
    int    m_genCounter = 0;  // 用于过滤过期 decodingFinished 信号

    // ===== 视频解码线程 =====
    QThread     *m_videoThread = nullptr;
    VideoWorker *m_videoWorker = nullptr;
};

#endif // MAINWINDOW_H

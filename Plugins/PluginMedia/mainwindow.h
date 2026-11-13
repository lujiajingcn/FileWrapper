#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QDebug>
#include <QTimer>
#include <QTime>
#include <QtGlobal>
#include <QAudioFormat>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QAudioSink>
#else
#include <QAudioOutput>
#endif
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

// Qt6 用 QAudioSink 取代 Qt5 的 QAudioOutput（推模式 PCM 输出）
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
typedef QAudioSink   AudioOutputDevice;
#else
typedef QAudioOutput AudioOutputDevice;
#endif

// 自定义内存 IO 的游标数据：AVIOContext 的 read/seek 回调通过它访问完整文件数据。
// 必须提供 seek 回调，否则 pb->seekable=0、avio_size() 失败，
// FLV / MPEG-TS / AVI / MP3 等容器会无法跳转或拿不到正确时长。
struct MemIOData
{
    unsigned char *data = nullptr;
    qint64 size = 0;
    qint64 pos  = 0;
};

// 音频输出通道数、采样率与采样格式（固定 stereo / S16 / 44100，与 QAudioFormat 配置一致）
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
    void audioPtsUpdated(qint64 ptsMs, int genId);  // 纯音频播放进度
    void decodingFinished(int genId);

public slots:
    void doDecode();
    void requestStop();
    void clearStopRequest();
    // 暂停/恢复解码线程的 PTS 帧调度（不退出循环，仅阻塞在条件变量上）
    void setPaused(bool paused);
    // 更新当前播放世代（用于 seek 时过滤排队的旧帧信号）
    void updateGenId(int genId);
    // 设置 seek 快进目标（绝对 PTS，毫秒）：>0 时丢弃目标前的包
    void setSeekTarget(qint64 absMs);

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

    // seek 后的快进目标（绝对 PTS 毫秒）；解码循环按它丢弃目标前的包
    std::atomic<qint64> m_seekTargetAbsMs{0};
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

    // 当前是否为纯音频播放（无视频流）
    bool isAudioOnly() const { return m_bAudioOnly; }

    // 由 PluginMedia::stopPlayback() 调用：彻底停止后台播放（停解码线程 + 停音频输出 + 释放 FFmpeg 上下文）。
    // 用于主程序切换文件时，确保被隐藏的旧视频 widget 不再在后台发声/解码。
    void stopPlayback();

public slots:
   void timeCallback(void);

protected:
   void resizeEvent(QResizeEvent*) override;

public slots:
   int showVideo(char *szFileData, qint64 nFileLen);
   // 大文件路径通道：由 PluginMedia::sendFileByPath 调用，复用 showVideo 的通用流程
   int showVideoFile(const QString &sPath);

private slots:
   void on_progressSlider_sliderPressed(void);
   void on_progressSlider_sliderMoved(int value);
   void on_progressSlider_sliderReleased(void);
   void on_playPauseBtn_clicked(void);
   void on_stopBtn_clicked(void);
   void on_volumeSlider_valueChanged(int value);

   // VideoWorker 信号槽
   void onFrameDecoded(QImage img, qint64 ptsMs, int genId);
   void onAudioPtsUpdated(qint64 ptsMs, int genId);  // 纯音频播放进度
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
    unsigned char   *m_avioBuffer = nullptr;          // 完整文件数据（内存 IO 的数据源）
    unsigned char   *m_avioInternalBuffer = nullptr;  // AVIOContext 内部读缓冲，由 avio_context_free 释放
    QString          m_sPendingPath;                   // 大文件按路径打开时暂存的文件路径
    MemIOData        m_memIO;                        // read / seek 回调使用的游标
    AVIOContext     *m_avioCtx   = nullptr;
    int videoindex = -1;

    int             audioindex = -1;
    AVCodecContext  *aCodecCtx  = nullptr;
    QByteArray      byteBuf;
    AudioOutputDevice *audioOutput = nullptr;
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
    bool   m_bAudioOnly = false;   // 纯音频播放（无视频流）
    qint64 m_nTargetMs = 0;
    qint64 m_nDurationMs = 0;
    qint64 m_nCurrentMs = 0;
    // 部分容器（MPEG-TS、MP3 等）的 PTS 不从 0 开始：进度显示要减去起始偏移，
    // seek 目标则要加上它，两者才能与 UI 的相对时间轴保持一致。
    qint64 m_nStartMs = 0;
    // >0 表示 seek 后正处于「回退到关键帧」阶段，进度条暂不接受更早的 PTS 覆盖，
    // 避免 AVSEEK_FLAG_BACKWARD 把刚拖到的位置又拉回开头。
    qint64 m_nSeekTargetMs = 0;
    int    m_nVolume = 80;
    int    m_genCounter = 0;  // 用于过滤过期 decodingFinished 信号

    // ===== 视频解码线程 =====
    QThread     *m_videoThread = nullptr;
    VideoWorker *m_videoWorker = nullptr;
};

#endif // MAINWINDOW_H

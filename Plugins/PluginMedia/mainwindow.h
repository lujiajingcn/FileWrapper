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
#include <memory>

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

// 音频缓冲上界：约 8 秒 PCM 数据（防止解码快于播放时无限增长 / 延迟累积）
// ASF/WMV 容器的音频包交错通常不如 MP4 均匀，需要更大的缓冲来吸收突发音频包，
// 避免缓冲区满后丢弃音频块导致"跳帧"。
// 2 通道 * 2 字节(S16) * 44100 * 8
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
    // 析构时兜底释放解码线程持有的 sws 上下文与输出缓冲
    ~VideoWorker() override;

    // 设置解码所需的 FFmpeg 上下文（主线程调用，doDecode 启动前）
    //
    // 注意：视频缩放上下文（sws）与 RGB 输出缓冲**不再**由主线程创建，
    // 改由解码线程自己拥有。原因是窗口缩放时输出尺寸必须跟着变，
    // 而重建 sws 上下文必须释放旧缓冲——若旧缓冲归主线程所有，
    // 主线程释放时解码线程可能正在 sws_scale，会造成 use-after-free。
    void setContexts(AVFormatContext *fmtCtx,
                     AVCodecContext *vCodecCtx, int videoIdx,
                     AVCodecContext *aCodecCtx, int audioIdx,
                     int width, int height,
                     AVPixelFormat srcPixFmt,
                     AVFrame *frame, AVFrame *audioFrame,
                     SwrContext *swr, uint8_t *aOutBuf,
                     std::shared_ptr<QMutex> aMutex, std::shared_ptr<QByteArray> aByteBuf,
                     int outSampleRate, int genId,
                     int audioOutBufCapacity,
                     quint64 runId);

    // 请求改变视频输出尺寸（主线程调用，窗口缩放时）。
    // 解码线程会在下一帧前在自己的线程内重建 sws 上下文与输出缓冲：
    // 既保证新帧按新尺寸输出，也不会出现「主线程释放、解码线程还在用」的 UAF。
    void requestOutputSize(int w, int h);

    // 阻塞等待 doDecode 退出（用于停止/seek/释放资源），超时返回 false
    bool waitForDecodingStopped(unsigned long timeoutMs = 2000);

signals:
    void frameDecoded(QImage img, qint64 ptsMs, int genId);
    void audioPtsUpdated(qint64 ptsMs, int genId);  // 纯音频播放进度
    void decodingFinished(int genId);

public slots:
    // P0-4：每个解码任务绑定创建时的 runId。旧 runId 的任务在入口直接退出，
    // 绝不读取/清理当前上下文（排队任务也不再能越过 stop 检查）。
    void doDecodeRun(quint64 runId);
    void requestStop();
    void clearStopRequest();
    // 上下文世代：主线程换上下文/停止时推进，使排队中的旧任务立即失效
    void setContextRunId(quint64 runId);
    // 暂停/恢复解码线程的 PTS 帧调度（不退出循环，仅阻塞在条件变量上）
    void setPaused(bool paused);
    // 更新当前播放世代（用于 seek 时过滤排队的旧帧信号）
    void updateGenId(int genId);
    // 设置 seek 快进目标（绝对 PTS，毫秒）：>0 时丢弃目标前的包
    void setSeekTarget(qint64 absMs);
    // 释放解码线程自己持有的 sws 上下文与 RGB 输出缓冲。
    // 只能在解码线程已停止后调用（否则会与正在进行的 sws_scale 竞争）。
    void releaseVideoResources();

    bool isDecodingActive() const { return m_running.load(std::memory_order_relaxed) != 0; }

    // 当前实际使用的视频输出尺寸（诊断/测试用：跨线程读取，仅用于观察）
    int currentOutW() const { return outW.load(std::memory_order_relaxed); }
    int currentOutH() const { return outH.load(std::memory_order_relaxed); }

private:
    // 在解码线程内重建视频缩放上下文与输出缓冲（输出尺寸或源参数变化时调用）。
    // 返回 false 表示本次重建失败、上下文不可用；调用方不应提交新的源参数。
    bool rebuildVideoSws(int srcW, int srcH, AVPixelFormat srcPixFmt, int outW, int outH);

private:
    AVFormatContext *pFormatCtx = nullptr;
    AVCodecContext  *pCodecCtx  = nullptr;  // video
    int videoindex = -1;

    AVCodecContext  *aCodecCtx  = nullptr;  // audio
    int audioindex = -1;

    int srcW = 0, srcH = 0;
    // 当前 sws 输出尺寸（= 生成的 QImage 尺寸）；原子量：主线程可无锁读取做诊断
    std::atomic<int> outW{0}, outH{0};
    AVPixelFormat m_srcPixFmt = AV_PIX_FMT_NONE;
    // 当前 sws 上下文对应的源参数：源尺寸/像素格式变化时必须重建，
    // 只比较输出尺寸会误判为"没变"而复用旧上下文。
    int           m_swsSrcW = 0;
    int           m_swsSrcH = 0;
    AVPixelFormat m_swsSrcFmt = AV_PIX_FMT_NONE;

    // 视频缩放上下文与 RGB 输出缓冲：由解码线程自己拥有。
    // 放在解码线程里，是为了让「窗口缩放 → 换输出尺寸」能在同一线程内完成：
    // 主线程只发一个尺寸请求，绝不碰正在被 sws_scale 使用的缓冲。
    SwsContext  *m_sws = nullptr;
    uint8_t     *m_outBuf = nullptr;
    int          m_outBufSize = 0;
    uint8_t     *m_outData[AV_NUM_DATA_POINTERS] = { nullptr };
    int          m_outLinesize[AV_NUM_DATA_POINTERS] = { 0 };
    // 主线程请求的输出尺寸：宽高打包进一个 64 位量，保证成对读到（不会读到新宽+旧高）
    std::atomic<quint64> m_reqOutSize{0};

    AVFrame         *pFrame = nullptr;

    SwrContext      *swr_ctx = nullptr;
    AVFrame         *pAudioFrame = nullptr;
    uint8_t         *audio_out_buffer = nullptr;
    // 音频缓存互斥锁与 PCM 缓冲改为 shared_ptr：解码线程持有一份所有权。
    // 原因：P0-5 的“停播未确认”兜底分支会刻意保留 worker/线程，而 MainWindow 对象
    // 仍会照常析构；若这里还是裸指针，worker 下一次写音频就会访问已销毁的成员
    // （use-after-free）。shared_ptr 让锁与缓冲活到 worker 真正退出为止，
    // 代价只是极端情况下随被保留的 worker 一起泄漏到进程退出。
    std::shared_ptr<QMutex>    m_audioMutex;
    std::shared_ptr<QByteArray> m_audioByteBuf;
    int m_outSampleRate = AUDIO_OUT_SAMPLE_RATE;
    int m_audioOutBufCapacity = 0;  // 音频输出缓冲区实际容量（样本数），用作 swr_convert 的 out_count

    int m_genId = -1;
    // P0-4：当前上下文对应的播放世代。doDecodeRun 的任务世代与它不一致时直接退出，
    // 用于隔离「已排队但尚未执行」的旧任务。
    std::atomic<quint64> m_contextRunId{0};

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

    // 诊断用：解码线程当前**实际**使用的视频输出尺寸。
    // 窗口缩放后它应随画面区尺寸一起变化（证明不是只在界面侧拉伸旧帧）。
    QSize decodedOutputSize() const;

    // 由 PluginMedia::stopPlayback() 调用：彻底停止后台播放（停解码线程 + 停音频输出 + 释放 FFmpeg 上下文）。
    // 用于主程序切换文件时，确保被隐藏的旧视频 widget 不再在后台发声/解码。
    void stopPlayback();
    bool stopPlaybackAndWait();
    // v7：非阻塞查询后台解码线程是否已退出（供宿主在 stop 超时后自动复查）。
    bool isPlaybackStopped() const;
    // 打开失败 / 0 字节时停止后台播放并清空画面，避免残留上一个文件
    void clearContent();

    // v6：打开失败的**真实原因**（空串 = 本次没失败 / 未提供）。
    // 主程序用它替代「插件无法显示该文件」这种笼统提示：没有音频输出设备、
    // 解码器不支持该编码、文件损坏在界面上必须能区分开。
    QString lastError() const { return m_sLastError; }

    // 把失败原因（含文件信息与建议）留在画面区。打开失败时画面区本来是黑的，
    // 只靠状态栏 10 秒的消息，用户回头看标签页就只剩一块黑屏。
    void showFailurePage();
    // 每次新的打开尝试开始时调用：宿主只会在 sendFile* 返回 false 后读 lastError()，
    // 陈旧的失败原因会让它把「这次」的失败解释成上一次的原因。
    void clearLastError();

public slots:
   void timeCallback(void);

protected:
   void resizeEvent(QResizeEvent*) override;

public slots:
   int showVideo(char *szFileData, qint64 nFileLen);
    // v4：归档内存映射入口（直接引用宿主 mmap 的只读内存，不复制整份文件）
    int showVideoMapped(char *szFileData, qint64 nFileLen);
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
    // force=true 时即使解码线程未确认退出也释放（仅析构使用）
    void clearFFmpegResources(bool force = false);
    // 统一的打开失败出口：记录真实原因 -> 释放 FFmpeg 资源 -> 返回 -1。
    // 所有 return -1 都必须走这里，否则宿主又只能显示笼统提示。
    int failOpen(const QString &sShortReason, const QString &sDetail = QString());
    // 启动音频输出。失败时通过 psShort/psDetail 给出可照做的真实原因
    // （没有输出设备 / 设备不支持该采样格式 / 设备打开失败是三种不同的问题）。
    bool startAudioOutput(const QAudioFormat &fmt, AVStream *astream,
                          QString *psShort, QString *psDetail);
    // Qt6 专用：QAudioSink 启动失败只给一个 OpenError，靠多媒体设备信息把原因说清楚
    void diagnoseAudioFailure(const QAudioFormat &fmt, int nErr, AVStream *astream,
                              QString *psShort, QString *psDetail) const;
    // 容器/流/码率三种口径估算时长（进度条与失败页共用，避免两处算法漂移）
    qint64 probeDurationMs(AVFormatContext *pCtx, AVStream *vstream, AVStream *astream) const;
    // 失败页用的文件信息：封装 + 音频编码 + 采样率/声道
    QString sourceSummary(AVFormatContext *pCtx, AVStream *astream) const;
    void doSeek(qint64 ms);
    void updateProgressDisplay();
    void updatePlayPauseIcon();
    // Stop 后 QAudioSink 进入 StoppedState、streamOut 失效：按需重新 start()，
    // 供 seek/重播/继续播放复用（resume() 不能从 StoppedState 恢复）。
    bool ensureAudioStarted();
    // 停止 worker 并阻塞等待其退出 doDecode；返回 false 表示超时、worker 仍在运行
    bool stopWorkerAndWait(unsigned long timeoutMs = 2000);
    // P0-4：推进播放世代并让 worker 立刻作废旧任务（排队任务在入口即退出）
    void invalidateDecodeRun();
    // 按指定世代排队一次解码任务
    void queueDecode(quint64 runId);
    // 根据源宽高与目标区域计算保持宽高比的目标输出尺寸
    QSize computeAspectRatioSize(int srcW, int srcH, int dstW, int dstH) const;
    // 清空画面区（清除残留 pixmap 与文字）：切换文件时必须调用，
    // 否则上一个文件的最后一帧会残留（例如 MP4 → WAV 切换时，
    // 纯音频没有任何视频帧到来，上一帧就一直停在画面上）。
    void clearVideoPicture();
    // 刷新画面区：清空画面后，纯音频文件补上提示文字。
    void refreshVideoArea();
    // 按当前画面区尺寸重算视频输出尺寸，并让解码线程切换到新尺寸。
    // 窗口缩放后必须调用，否则画面尺寸会停留在打开文件时的取值（不跟随界面）。
    void updateVideoOutputSize();

private:
    Ui::MainWindow *ui;
    QTimer *timer;

    // 当前视频输出尺寸（已按源宽高比适配到画面区）。
    // 解码线程按它输出帧；窗口缩放后由 updateVideoOutputSize() 更新。
    QSize m_curVideoOutSize;
    // 是否有一次「窗口缩放后的尺寸更新」已排队（用于合并连续的 resize 事件）
    bool m_resizeUpdatePending = false;

    AVFormatContext *pFormatCtx = nullptr;
    AVCodecContext  *pCodecCtx  = nullptr;
    AVFrame         *pFrame     = nullptr;
    AVFrame         *pAudioFrame = nullptr;
    unsigned char   *m_avioBuffer = nullptr;          // 完整文件数据（内存 IO 的数据源）
    unsigned char   *m_avioInternalBuffer = nullptr;  // AVIOContext 内部读缓冲，由 avio_context_free 释放
    QString          m_sPendingPath;                   // 大文件按路径打开时暂存的文件路径
    bool             m_bMappedInput = false;           // 本次 showVideo 使用宿主内存映射（不复制、不释放）
    MemIOData        m_memIO;                        // read / seek 回调使用的游标
    AVIOContext     *m_avioCtx   = nullptr;
    int videoindex = -1;

    int             audioindex = -1;
    AVCodecContext  *aCodecCtx  = nullptr;
    // PCM 缓存与互斥锁：shared_ptr，解码线程共享所有权（见 VideoWorker 同名成员的注释）。
    // 不再作为裸成员被子窗口析构销毁，避免“保留 worker”分支留下悬空指针。
    std::shared_ptr<QByteArray> byteBuf = std::make_shared<QByteArray>();
    AudioOutputDevice *audioOutput = nullptr;
    QIODevice       *streamOut = nullptr;

    // ===== 音频重采样 =====
    SwrContext        *swr_ctx = nullptr;
    uint8_t           *audio_out_buffer = nullptr;
    AVChannelLayout    out_ch_layout;

    // 音频缓存互斥锁（shared_ptr 理由同上）
    std::shared_ptr<QMutex> m_audioMutex = std::make_shared<QMutex>();

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
    int    m_genCounter = 0;  // 用于过滤过期 decodingFinished / frameDecoded 信号
    // P0-4：宿主侧播放世代（与 worker 的 m_contextRunId 对应）
    quint64 m_runId = 0;
    // P1-18：FFmpeg 中断回调共享标志（停止/换文件时置 1，让阻塞 I/O 尽快返回）
    std::atomic<int> m_ioInterrupt{0};

    // ===== 视频解码线程 =====
    QThread     *m_videoThread = nullptr;
    VideoWorker *m_videoWorker = nullptr;
    // 解码线程超时未退出时挂起资源释放，等 decodingFinished 后再清理，避免 UAF
    bool         m_bCleanupPending = false;

    // 最近一次打开失败的说明（v6：交给宿主显示，取代「插件无法显示该文件」）
    QString      m_sLastError;       // 一句话，供状态栏/对话框拼接
    QString      m_sFailureDetail;   // 多行：原因 + 文件信息 + 建议（画面区失败页）
};

#endif // MAINWINDOW_H

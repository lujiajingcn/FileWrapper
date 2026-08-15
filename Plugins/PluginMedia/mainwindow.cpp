#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QFile>
#include <QDebug>
#include <QTimer>
#include <QSlider>
#include <QDateTime>
#include <QCoreApplication>
#include <QThread>
#include <QMutexLocker>
#include <QElapsedTimer>
#include <QSize>

// ============================================================
// VideoWorker 实现 — 运行在独立线程，负责所有解码 + 帧调度
// ============================================================

VideoWorker::VideoWorker(QObject *parent)
    : QObject(parent), m_lastPtsMs(0)
{
}

void VideoWorker::setContexts(AVFormatContext *fmtCtx,
                              AVCodecContext *vCodecCtx, int videoIdx,
                              AVCodecContext *aCodecCtx, int audioIdx,
                              int width, int height,
                              int outWidth, int outHeight,
                              SwsContext *swsCtx,
                              AVFrame *frame, AVFrame *frameRGB, AVFrame *audioFrame,
                              uint8_t *outBuf,
                              SwrContext *swr,
                              uint8_t *aOutBuf,
                              QMutex *aMutex,
                              QByteArray *aByteBuf,
                              int outSampleRate, int genId)
{
    QMutexLocker locker(&m_mutex);
    pFormatCtx = fmtCtx;
    pCodecCtx  = vCodecCtx;
    videoindex = videoIdx;
    this->aCodecCtx  = aCodecCtx;
    audioindex = audioIdx;
    srcW = width;
    srcH = height;
    outW = outWidth;
    outH = outHeight;
    imgCtx = swsCtx;
    pFrame = frame;
    pFrameRGB = frameRGB;
    pAudioFrame = audioFrame;
    out_buffer = outBuf;
    swr_ctx = swr;
    audio_out_buffer = aOutBuf;
    m_audioMutex = aMutex;
    m_audioByteBuf = aByteBuf;
    m_outSampleRate = outSampleRate;
    m_genId = genId;
    // 新播放开始时清除 stop 与 pause 标志
    m_stopRequested.store(0, std::memory_order_relaxed);
    m_paused.store(0, std::memory_order_relaxed);
    m_pauseCond.wakeAll();
}

void VideoWorker::requestStop()
{
    m_stopRequested.store(1, std::memory_order_relaxed);
    // 唤醒可能休眠在暂停条件变量上的循环，以便其检测到 stop 并退出
    QMutexLocker locker(&m_mutex);
    m_pauseCond.wakeAll();
}

void VideoWorker::clearStopRequest()
{
    m_stopRequested.store(0, std::memory_order_relaxed);
}

void VideoWorker::setPaused(bool paused)
{
    m_paused.store(paused ? 1 : 0, std::memory_order_relaxed);
    QMutexLocker locker(&m_mutex);
    m_pauseCond.wakeAll();
}

bool VideoWorker::waitForDecodingStopped(unsigned long timeoutMs)
{
    QMutexLocker locker(&m_mutex);
    if (m_running.load(std::memory_order_relaxed) == 0)
        return true;
    return m_stoppedCond.wait(&m_mutex, timeoutMs);
}

void VideoWorker::doDecode()
{
    // 启动新一轮解码：标记运行中
    // 注意：不在此清除 stop 标志。若主线程已请求停止（如 clearFFmpegResources），
    // 则此处仍排队的 doDecode 会立即检测到 stop 并退出，避免访问已释放的资源。
    // stop 标志由 setContexts() 在新播放开始时清除。
    m_running.store(1, std::memory_order_relaxed);
    m_lastPtsMs = 0;

    // 本线程持有的数据包（与主线程解耦，避免跨线程共享所有权）
    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
    {
        m_running.store(0, std::memory_order_relaxed);
        m_stoppedCond.wakeAll();
        emit decodingFinished(m_genId);
        return;
    }

    // 播放时钟：从第一帧开始计时，用于音视频同步
    QElapsedTimer playClock;
    bool clockStarted = false;
    // 第一帧的 PTS（绝对值），用作同步基准。targetMs = ptsMs - firstPtsMs，
    // 这样无论 PTS 从 0 还是其他值开始（如 seek 后），等待时间都是相对偏移。
    qint64 firstPtsMs = -1;
    // 暂停期间累计的真实流逝时间（微秒），用于恢复播放后校正时钟，避免跳变
    qint64 pausedClockUs = 0;
    bool  wasPausedAtLoop = false;

    while (true)
    {
        if (m_stopRequested.load(std::memory_order_relaxed))
            goto stop_decode;

        // ---- 暂停处理：阻塞在条件变量上，直到被唤醒（继续/停止） ----
        if (m_paused.load(std::memory_order_relaxed) && !wasPausedAtLoop)
        {
            QMutexLocker locker(&m_mutex);
            qint64 pauseBegin = playClock.isValid() ? playClock.nsecsElapsed() / 1000 : 0;
            while (m_paused.load(std::memory_order_relaxed)
                   && !m_stopRequested.load(std::memory_order_relaxed))
            {
                m_pauseCond.wait(&m_mutex);
            }
            if (m_stopRequested.load(std::memory_order_relaxed))
                goto stop_decode;
            // 记下本次暂停持续的时间
            qint64 pauseEnd = playClock.nsecsElapsed() / 1000;
            pausedClockUs += (pauseEnd - pauseBegin);
            wasPausedAtLoop = true;
        }
        wasPausedAtLoop = false;

        if (!pFormatCtx || videoindex < 0)
            break;

        int ret = av_read_frame(pFormatCtx, pkt);
        if (ret < 0)
        {
            // 播放结束或 EOF
            break;
        }

        if (pkt->stream_index == videoindex)
        {
            // ====== 视频解码 ======
            ret = avcodec_send_packet(pCodecCtx, pkt);
            if (ret >= 0)
            {
                while (ret >= 0)
                {
                    ret = avcodec_receive_frame(pCodecCtx, pFrame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    if (ret < 0)
                    {
                        { char errbuf[128]; av_strerror(ret, errbuf, sizeof(errbuf));
                        qWarning() << "[VideoWorker] 视频解码失败:" << errbuf; }
                        break;
                    }

                    // 像素格式转换 + 直接缩放到目标尺寸（保持宽高比由 sws 上下文保证）
                    if (imgCtx)
                        sws_scale(imgCtx, (const unsigned char* const*)pFrame->data,
                                  pFrame->linesize, 0, srcH,
                                  pFrameRGB->data, pFrameRGB->linesize);

                    // 构建 QImage（数据已为目标尺寸 outW x outH）
                    QImage img((uchar*)pFrameRGB->data[0], outW, outH, QImage::Format_RGB32);
                    img = img.copy(); // 深拷贝，安全传递到主线程

                    // 计算 PTS
                    qint64 ptsMs = 0;
                    if (pFrame->pts != AV_NOPTS_VALUE)
                    {
                        double secs = pFrame->pts * av_q2d(pFormatCtx->streams[videoindex]->time_base);
                        ptsMs = (qint64)(secs * 1000);
                        if (ptsMs < 0) ptsMs = 0;
                    }

                    emit frameDecoded(img, ptsMs);

                    if (ptsMs > 0)
                        m_lastPtsMs = ptsMs;

                    // === 帧调度：基于 PTS 的播放时钟同步（不阻塞 UI 主线程） ===
                    if (!clockStarted)
                    {
                        playClock.start();
                        firstPtsMs = ptsMs;  // 记录第一帧 PTS 作为同步基准
                        clockStarted = true;
                    }

                    // 当前播放位置 = 真实流逝时间刨去暂停期间
                    qint64 clockUs = playClock.nsecsElapsed() / 1000 - pausedClockUs;
                    qint64 clockMs = clockUs / 1000;
                    // 目标时间 = 相对于第一帧的 PTS 偏移（避免 seek 后 PTS 绝对值过大导致 sleep 几十秒）
                    qint64 targetMs = ptsMs - firstPtsMs;

                    if (targetMs > clockMs)
                    {
                        // 提前了：等待到目标时刻（用 usleep 提高精度）
                        qint64 waitUs = (targetMs - clockMs) * 1000;
                        // 分片等待以便及时响应 stop/pause，但保持足够精度
                        while (waitUs > 0)
                        {
                            if (m_stopRequested.load(std::memory_order_relaxed))
                                goto stop_decode;
                            if (m_paused.load(std::memory_order_relaxed))
                                break; // 暂停已置位，回到外层暂停处理
                            qint64 chunk = qMin<qint64>(waitUs, 2000); // 2ms 一片
                            QThread::usleep((unsigned long)chunk);
                            waitUs -= chunk;
                        }
                    }
                    // else: 已经落后（ptsMs <= clockMs），不等待直接解码下一帧（追赶）

                    // 若等待中检测到暂停，立即回到外层循环进入暂停处理
                    if (m_paused.load(std::memory_order_relaxed))
                        break;
                }
            }
        }
        else if (audioindex >= 0 && pkt->stream_index == audioindex)
        {
            // ====== 音频解码 ======
            ret = avcodec_send_packet(aCodecCtx, pkt);
            if (ret >= 0)
            {
                while (ret >= 0)
                {
                    ret = avcodec_receive_frame(aCodecCtx, pAudioFrame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    if (ret < 0)
                    {
                        { char errbuf[128]; av_strerror(ret, errbuf, sizeof(errbuf));
                        qWarning() << "[VideoWorker] 音频解码失败:" << errbuf; }
                        break;
                    }

                    if (swr_ctx)
                    {
                        int len = swr_convert(swr_ctx, &audio_out_buffer, m_outSampleRate,
                                              (const uint8_t **)pAudioFrame->data, pAudioFrame->nb_samples);
                        if (len > 0)
                        {
                            // 输出已重采样为 stereo (2ch S16)，每样本 2 字节
                            int dst_bufsize = av_samples_get_buffer_size(nullptr, AUDIO_OUT_CHANNELS,
                                                                         len, AUDIO_OUT_FORMAT, 1);
                            if (dst_bufsize > 0)
                            {
                                // 非阻塞追加：缓冲有空间则追加音频数据，否则丢弃当前块。
                                // 不在此处 sleep——否则会阻塞同一循环中的视频包读取和解码，
                                // 导致视频帧卡顿。音频 pacing 由主线程消费速率自然控制。
                                QMutexLocker locker(m_audioMutex);
                                int buffered = m_audioByteBuf->size();
                                if (buffered + dst_bufsize <= AUDIO_BUFFER_LIMIT_BYTES
                                    // 单个音频块超过上限时，缓冲为空也允许写入（否则永远无法播放）
                                    || (dst_bufsize > AUDIO_BUFFER_LIMIT_BYTES && buffered == 0))
                                {
                                    m_audioByteBuf->append(
                                        reinterpret_cast<const char *>(audio_out_buffer), dst_bufsize);
                                }
                                // 缓冲已满 → 丢弃当前块（demux 快于实时时正常限流）
                            }
                        }
                    }
                }
            }
        }

        av_packet_unref(pkt);

        if (m_stopRequested.load(std::memory_order_relaxed))
            break;
    }

stop_decode:
    {
        av_packet_free(&pkt);
        {
            QMutexLocker locker(&m_mutex);
            m_stopRequested.store(0, std::memory_order_relaxed);
            m_paused.store(0, std::memory_order_relaxed);
            m_running.store(0, std::memory_order_relaxed);
            m_stoppedCond.wakeAll();
        }
        emit decodingFinished(m_genId);
    }
}

// ============================================================
// MainWindow 实现
// ============================================================

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , audioOutput(nullptr)
    , streamOut(nullptr)
    , m_bPaused(false)
    , m_videoThread(nullptr)
    , m_videoWorker(nullptr)
{
    ui->setupUi(this);

    // 初始化音频通道布局
    av_channel_layout_default(&out_ch_layout, AUDIO_OUT_CHANNELS);

    ui->progressSlider->setRange(0, 100);
    ui->progressSlider->setEnabled(false);

    // 设置播放/暂停按钮初始状态
    ui->playPauseBtn->setText("⏸");

    timer = new QTimer(this);
    timer->setTimerType(Qt::PreciseTimer);
    connect(timer, &QTimer::timeout, this, &MainWindow::timeCallback);

    // 创建视频解码线程
    m_videoThread = new QThread(this);
    m_videoWorker = new VideoWorker();
    m_videoWorker->moveToThread(m_videoThread);

    connect(m_videoWorker, &VideoWorker::frameDecoded,
            this, &MainWindow::onFrameDecoded, Qt::QueuedConnection);
    connect(m_videoWorker, &VideoWorker::decodingFinished,
            this, &MainWindow::onDecodingFinished, Qt::QueuedConnection);

    m_videoThread->start();
}

void MainWindow::timeCallback(void)
{
    // 1. 优先处理 seek
    if (m_bSeekPending)
    {
        m_bSeekPending = false;
        qint64 target = m_nTargetMs;
        m_nTargetMs = 0;
        doSeek(target);
    }

    // 2. 音频缓存播放（线程安全）
    if (audioOutput && streamOut && audioOutput->state() != QAudio::StoppedState
     && audioOutput->state() != QAudio::SuspendedState)
    {
        QMutexLocker locker(&m_audioMutex);
        int writeBytes = qMin(byteBuf.length(), audioOutput->bytesFree());
        if (writeBytes > 0)
        {
            streamOut->write(byteBuf.data(), writeBytes);
            if (writeBytes < byteBuf.length())
                byteBuf = byteBuf.right(byteBuf.length() - writeBytes);
            else
                byteBuf.clear();
        }
    }

    // 3. 更新 UI 显示
    updateProgressDisplay();
}

void MainWindow::onFrameDecoded(QImage img, qint64 ptsMs)
{
    if (!m_bPlaying || m_bPaused)
        return;

    QPixmap temp = QPixmap::fromImage(img);
    if (!temp.isNull())
        ui->videoLabel->setPixmap(temp);

    if (ptsMs > 0)
    {
        m_nCurrentMs = ptsMs;
        if (m_nCurrentMs > m_nDurationMs)
            m_nCurrentMs = m_nDurationMs;
    }
}

void MainWindow::onDecodingFinished(int genId)
{
    // 过滤过期的信号：只有当 genId 与当前 m_genCounter 一致时，才处理
    if (genId != m_genCounter)
        return;

    // 正常播放结束（非暂停/非拖动/非seek）
    if (m_bPlaying && !m_bPaused && !m_bUserDragging && !m_bSeekPending)
    {
        m_bPlaying = false;
        m_bPaused = false;
        updatePlayPauseIcon();

        if (timer->isActive())
            timer->stop();
    }
}

void MainWindow::resizeEvent(QResizeEvent*)
{
    // 仅记录目标区域；实际重建 sws 上下文在下一帧刷新时按新尺寸进行，
    // 避免在 resize 风暴中频繁重新分配（可简单在 onFrameDecoded 中判断尺寸变化）。
    videoW = ui->videoLabel->size().width();
    videoH = ui->videoLabel->size().height();
}

void MainWindow::stopWorkerAndWait(unsigned long timeoutMs)
{
    if (!m_videoWorker)
        return;
    // 请求停止（DirectConnection 设置原子标志并唤醒暂停条件变量）
    QMetaObject::invokeMethod(m_videoWorker, "requestStop", Qt::DirectConnection);
    // 阻塞等待 worker 退出 doDecode（条件变量替代旧的忙等待轮询）
    m_videoWorker->waitForDecodingStopped(timeoutMs);
}

void MainWindow::clearFFmpegResources()
{
    // 停止视频解码线程
    stopWorkerAndWait(2000);

    if (timer->isActive()) timer->stop();

    if (audioOutput) { delete audioOutput; audioOutput = nullptr; streamOut = nullptr; }

    if (swr_ctx)          { swr_free(&swr_ctx); swr_ctx = nullptr; }
    if (audio_out_buffer) { av_freep(&audio_out_buffer); audio_out_buffer = nullptr; }
    if (img_convert_ctx)  { sws_freeContext(img_convert_ctx); img_convert_ctx = nullptr; }
    if (out_buffer)       { av_freep(&out_buffer); out_buffer = nullptr; }
    av_channel_layout_uninit(&out_ch_layout);
    av_channel_layout_default(&out_ch_layout, AUDIO_OUT_CHANNELS);

    if (pFrameRGB)   { av_frame_free(&pFrameRGB);   pFrameRGB   = nullptr; }
    if (pFrame)      { av_frame_free(&pFrame);      pFrame      = nullptr; }
    if (pAudioFrame) { av_frame_free(&pAudioFrame); pAudioFrame = nullptr; }
    if (pCodecCtx)  { avcodec_free_context(&pCodecCtx);  pCodecCtx = nullptr; }
    if (aCodecCtx)  { avcodec_free_context(&aCodecCtx);  aCodecCtx = nullptr; }
    if (pFormatCtx) { avformat_close_input(&pFormatCtx); pFormatCtx = nullptr; }
    // m_avioCtx 释放时会连带释放其内部 buffer，因此这里不再单独 av_freep(m_avioBuffer)，
    // 否则会导致双重释放崩溃。
    if (m_avioCtx)  { avio_context_free(&m_avioCtx); m_avioCtx = nullptr; }
    m_avioBuffer = nullptr;

    videoindex = -1;
    audioindex = -1;
    m_bPlaying = false;
    m_bPaused = false;

    {
        QMutexLocker locker(&m_audioMutex);
        byteBuf.clear();
    }
}

MainWindow::~MainWindow()
{
    if (timer->isActive()) timer->stop();

    // 停止视频线程
    if (m_videoWorker)
    {
        QMetaObject::invokeMethod(m_videoWorker, "requestStop", Qt::DirectConnection);
    }
    if (m_videoThread && m_videoThread->isRunning())
    {
        m_videoThread->quit();
        m_videoThread->wait(3000);
    }

    clearFFmpegResources();
    delete ui;
}

void MainWindow::updateProgressDisplay(void)
{
    // 显示格式 m:ss / m:ss (VLC 风格)
    int curMin = (int)m_nCurrentMs / 60000;
    int curSec = ((int)m_nCurrentMs % 60000) / 1000;
    int totalMin = (int)m_nDurationMs / 60000;
    int totalSec = ((int)m_nDurationMs % 60000) / 1000;

    QString timeText = QString("%1:%2 / %3:%4")
        .arg(curMin)
        .arg(curSec, 2, 10, QChar('0'))
        .arg(totalMin)
        .arg(totalSec, 2, 10, QChar('0'));

    if (timeText != ui->timeLabel->text())
        ui->timeLabel->setText(timeText);

    // 程序更新进度条（避免触发 seek 信号）
    if (!m_bUserDragging)
    {
        ui->progressSlider->blockSignals(true);
        ui->progressSlider->setValue((int)m_nCurrentMs);
        ui->progressSlider->blockSignals(false);
    }
}

// ====== 进度条拖动手柄 ======
void MainWindow::on_progressSlider_sliderPressed(void)
{
    m_bUserDragging = true;
    if (timer->isActive()) timer->stop();
}

void MainWindow::on_progressSlider_sliderMoved(int value)
{
    // 拖动时实时显示时间
    int min = value / 60000;
    int sec = (value % 60000) / 1000;
    int totalMin = (int)m_nDurationMs / 60000;
    int totalSec = ((int)m_nDurationMs % 60000) / 1000;

    QString timeText = QString("%1:%2 / %3:%4")
        .arg(min)
        .arg(sec, 2, 10, QChar('0'))
        .arg(totalMin)
        .arg(totalSec, 2, 10, QChar('0'));
    ui->timeLabel->setText(timeText);
}

void MainWindow::on_progressSlider_sliderReleased(void)
{
    m_bUserDragging = false;
    m_bSeekPending = true;
    m_nTargetMs = ui->progressSlider->value();
    qDebug() << "[Seek] sliderReleased, target=" << m_nTargetMs << "ms";

    // 重新启动定时器以执行 seek 并继续播放
    timer->start(20);
}

// ====== 播放/暂停 ======
void MainWindow::on_playPauseBtn_clicked(void)
{
    if (!m_bPlaying) return;

    m_bPaused = !m_bPaused;
    updatePlayPauseIcon();

    if (m_bPaused)
    {
        // 暂停：暂停 worker 帧调度 + 停止定时器 + 暂停音频
        QMetaObject::invokeMethod(m_videoWorker, "setPaused", Qt::QueuedConnection, Q_ARG(bool, true));
        if (timer->isActive()) timer->stop();
        if (audioOutput) audioOutput->suspend();
        qDebug() << "[Play/Pause] Paused";
    }
    else
    {
        // 继续播放
        QMetaObject::invokeMethod(m_videoWorker, "setPaused", Qt::QueuedConnection, Q_ARG(bool, false));
        if (!timer->isActive()) timer->start(20);
        if (audioOutput) audioOutput->resume();
        qDebug() << "[Play/Pause] Playing";
    }
}

void MainWindow::updatePlayPauseIcon()
{
    if (m_bPaused)
        ui->playPauseBtn->setText("▶");
    else
        ui->playPauseBtn->setText("⏸");
}

// ====== 停止 ======
void MainWindow::on_stopBtn_clicked(void)
{
    if (timer->isActive()) timer->stop();
    m_bPlaying = false;
    m_bPaused = false;
    updatePlayPauseIcon();

    // 清空视频画面
    ui->videoLabel->clear();

    // ====== 先停止 worker，再操作 FFmpeg 上下文，避免数据竞争导致崩溃 ======
    stopWorkerAndWait(2000);

    // 重置进度（worker 已停止，此时操作上下文安全）
    if (pFormatCtx)
    {
        av_seek_frame(pFormatCtx, -1, 0, AVSEEK_FLAG_BACKWARD);
        if (pCodecCtx) avcodec_flush_buffers(pCodecCtx);
        if (aCodecCtx) avcodec_flush_buffers(aCodecCtx);
        QMutexLocker locker(&m_audioMutex);
        byteBuf.clear();
    }

    m_nCurrentMs = 0;
    ui->progressSlider->blockSignals(true);
    ui->progressSlider->setValue(0);
    ui->progressSlider->blockSignals(false);
    ui->timeLabel->setText(QString("0:00 / %1:%2")
        .arg((int)m_nDurationMs / 60000)
        .arg(((int)m_nDurationMs % 60000) / 1000, 2, 10, QChar('0')));

    if (audioOutput) audioOutput->stop();

    // 清除 stop 标志，允许后续恢复播放（doSeek / 重新 play）
    if (m_videoWorker)
    {
        QMetaObject::invokeMethod(m_videoWorker, "clearStopRequest", Qt::DirectConnection);
        QMetaObject::invokeMethod(m_videoWorker, "setPaused", Qt::DirectConnection, Q_ARG(bool, false));
    }

    qDebug() << "[Stop] Stopped";
}

// ====== 音量控制 ======
void MainWindow::on_volumeSlider_valueChanged(int value)
{
    m_nVolume = value;
    if (audioOutput)
    {
        audioOutput->setVolume(value / 100.0f);
    }
    qDebug() << "[Volume]" << value;
}

// ====== 执行 seek ======
void MainWindow::doSeek(qint64 ms)
{
    if (!pFormatCtx || videoindex < 0)
    {
        m_bPlaying = false;
        return;
    }
    if (ms < 0) ms = 0;
    if (ms > m_nDurationMs) ms = m_nDurationMs;

    m_bPlaying = false;
    m_bPaused = false;
    updatePlayPauseIcon();
    timer->stop();

    // 暂停视频解码线程（避免 seek 期间继续读帧）
    stopWorkerAndWait(2000);

    // stream_index = -1 时，av_seek_frame 期望时间戳单位为 AV_TIME_BASE (微秒)
    int64_t target_ts_us = ms * 1000;
    qDebug() << "[Seek] doSeek to" << ms << "ms (ts=" << target_ts_us << "us)";

    // 执行 seek（回退到关键帧，避免花屏）
    int seek_ret = av_seek_frame(pFormatCtx, -1, target_ts_us, AVSEEK_FLAG_BACKWARD);
    if (seek_ret < 0)
    {
        { char errbuf[128]; av_strerror(seek_ret, errbuf, sizeof(errbuf));
        qWarning() << "[Seek] Seek failed at" << ms << "ms:" << errbuf; }
    }

    // 清理解码器内部缓冲，从关键帧开始重新解码
    if (pCodecCtx) avcodec_flush_buffers(pCodecCtx);
    if (aCodecCtx) avcodec_flush_buffers(aCodecCtx);

    // 清空音频缓存
    {
        QMutexLocker locker(&m_audioMutex);
        byteBuf.clear();
    }

    m_nCurrentMs = ms;
    m_bPlaying = true;

    if (audioOutput) audioOutput->resume();

    // 清除 stop 标志，然后重新启动视频解码线程
    QMetaObject::invokeMethod(m_videoWorker, "clearStopRequest", Qt::DirectConnection);
    QMetaObject::invokeMethod(m_videoWorker, "setPaused", Qt::DirectConnection, Q_ARG(bool, false));
    QMetaObject::invokeMethod(m_videoWorker, "doDecode", Qt::QueuedConnection);

    timer->start(20);
}

// 计算保持宽高比的目标输出尺寸（letterbox，竖条/横条留白由 Label 对齐控制）
QSize MainWindow::computeAspectRatioSize(int srcW, int srcH, int dstW, int dstH) const
{
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0)
        return QSize(dstW, dstH);
    double ratio = (double)srcW / (double)srcH;
    double outRatio = (double)dstW / (double)dstH;

    int w = dstW, h = dstH;
    if (ratio > outRatio)
    {
        // 源更宽：以宽为准
        w = dstW;
        h = (int)(dstW / ratio + 0.5);
        if (h > dstH) { h = dstH; w = (int)(dstH * ratio + 0.5); }
    }
    else
    {
        // 源更高或相等：以高为准
        h = dstH;
        w = (int)(dstH * ratio + 0.5);
        if (w > dstW) { w = dstW; h = (int)(dstW / ratio + 0.5); }
    }
    return QSize(w, h);
}

// （重新）创建 sws 上下文：将源帧缩放为 outW x outH 的 RGB32
void MainWindow::rebuildSwsContext(int srcW, int srcH, AVPixelFormat srcPixFmt, int outW, int outH)
{
    if (outW <= 0 || outH <= 0 || !pFrameRGB)
        return;

    // 若已存在同尺寸上下文则复用
    if (img_convert_ctx && videoW == outW && videoH == outH)
        return;

    if (img_convert_ctx) { sws_freeContext(img_convert_ctx); img_convert_ctx = nullptr; }

    int new_buffer_size = av_image_get_buffer_size(AV_PIX_FMT_RGB32, outW, outH, 1);
    if (new_buffer_size < 0)
    {
        qWarning() << "[rebuildSwsContext] 计算输出缓冲大小失败.";
        return;
    }
    if (!out_buffer || out_buffer_size < new_buffer_size)
    {
        av_freep(&out_buffer);
        out_buffer = (unsigned char *)av_malloc(new_buffer_size);
        out_buffer_size = new_buffer_size;
    }
    if (!out_buffer)
    {
        qWarning() << "[rebuildSwsContext] 输出缓冲分配失败.";
        return;
    }

    av_image_fill_arrays(pFrameRGB->data, pFrameRGB->linesize, out_buffer,
                         AV_PIX_FMT_RGB32, outW, outH, 1);

    img_convert_ctx = sws_getContext(srcW, srcH, srcPixFmt,
                                     outW, outH, AV_PIX_FMT_RGB32,
                                     SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    videoW = outW;
    videoH = outH;
}

int MainWindow::showVideo(char *szFileData, qint64 nFileLen)
{
    if (szFileData == nullptr || nFileLen <= 0)
        return -1;

    if (timer->isActive()) timer->stop();

    // 清理上次播放的资源（包含停止视频解码线程）
    clearFFmpegResources();
    av_channel_layout_default(&out_ch_layout, AUDIO_OUT_CHANNELS);

    // 音频输出设置
    QAudioFormat fmt;
    fmt.setSampleRate(AUDIO_OUT_SAMPLE_RATE);
    fmt.setSampleSize(16);
    fmt.setChannelCount(AUDIO_OUT_CHANNELS);
    fmt.setCodec("audio/pcm");
    fmt.setByteOrder(QAudioFormat::LittleEndian);
    fmt.setSampleType(QAudioFormat::SignedInt);
    audioOutput = new QAudioOutput(fmt);
    audioOutput->setVolume(m_nVolume / 100.0f);
    streamOut = audioOutput->start();

    pFormatCtx = avformat_alloc_context();
    if (!pFormatCtx)
    {
        qWarning() << "[ShowVideo] avformat_alloc_context 失败.";
        clearFFmpegResources();
        return -1;
    }

    // 将传入的数据拷贝到 AVIOContext（内存播放，无需网络初始化）
    m_avioBuffer = (unsigned char *)av_malloc(nFileLen);
    if (!m_avioBuffer)
    {
        qWarning() << "[ShowVideo] AVIO buffer allocation failed.";
        clearFFmpegResources();
        return -1;
    }
    memcpy(m_avioBuffer, szFileData, nFileLen);
    m_avioCtx = avio_alloc_context(m_avioBuffer, nFileLen, 0, nullptr, nullptr, nullptr, nullptr);
    if (!m_avioCtx)
    {
        qWarning() << "[ShowVideo] AVIO context allocation failed.";
        clearFFmpegResources();
        return -1;
    }
    pFormatCtx->pb = m_avioCtx;
    // 标记自定义 IO：avformat_close_input 将不负责释放 pb，
    // 由我们手动 avio_context_free 释放，避免双重释放导致崩溃。
    pFormatCtx->flags |= AVFMT_FLAG_CUSTOM_IO;

    if (avformat_open_input(&pFormatCtx, nullptr, nullptr, nullptr) != 0)
    {
        qWarning() << "[ShowVideo] 无法打开输入流.";
        clearFFmpegResources();
        return -1;
    }

    if (avformat_find_stream_info(pFormatCtx, nullptr) < 0)
    {
        qWarning() << "[ShowVideo] 媒体流获取失败.";
        clearFFmpegResources();
        return -1;
    }

    // 用 av_find_best_stream 选择默认/最佳视频与音频流（比首个匹配更稳健）
    AVStream *vstream = nullptr, *astream = nullptr;
    videoindex = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audioindex = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_AUDIO, -1, videoindex, nullptr, 0);
    if (videoindex >= 0)
        vstream = pFormatCtx->streams[videoindex];
    if (audioindex >= 0)
        astream  = pFormatCtx->streams[audioindex];

    if (videoindex < 0)
    {
        qWarning() << "[ShowVideo] 找不到视频流.";
        clearFFmpegResources();
        return -1;
    }
    if (audioindex < 0)
    {
        qWarning() << "[ShowVideo] 找不到音频流.";
        clearFFmpegResources();
        return -1;
    }

    // ========== 视频解码 ==========
    pCodecCtx = avcodec_alloc_context3(nullptr);
    if (!pCodecCtx || avcodec_parameters_to_context(pCodecCtx, vstream->codecpar) < 0)
    {
        qWarning() << "[ShowVideo] 视频解码器上下文初始化失败.";
        clearFFmpegResources();
        return -1;
    }

    // 启用多线程解码（大文件/高分辨率下显著提升流畅度）
    // threads=0 让 FFmpeg 根据 CPU 核心数自动选择线程数
    // 同时允许帧级和分片级线程，由解码器自行选择其支持的模式
    pCodecCtx->thread_count = 0;
    pCodecCtx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

    const AVCodec *videoCodec = avcodec_find_decoder(pCodecCtx->codec_id);
    if (!videoCodec)
    {
        qWarning() << "[ShowVideo] 找不到视频解码器.";
        clearFFmpegResources();
        return -1;
    }
    if (avcodec_open2(pCodecCtx, videoCodec, nullptr) < 0)
    {
        qWarning() << "[ShowVideo] 打开视频解码器失败.";
        clearFFmpegResources();
        return -1;
    }

    // ========== 音频解码 ==========
    aCodecCtx = avcodec_alloc_context3(nullptr);
    if (!aCodecCtx || avcodec_parameters_to_context(aCodecCtx, astream->codecpar) < 0)
    {
        qWarning() << "[ShowVideo] 音频解码器上下文初始化失败.";
        clearFFmpegResources();
        return -1;
    }

    const AVCodec *audioCodec = avcodec_find_decoder(aCodecCtx->codec_id);
    if (!audioCodec)
    {
        qWarning() << "[ShowVideo] 找不到音频解码器.";
        clearFFmpegResources();
        return -1;
    }
    if (avcodec_open2(aCodecCtx, audioCodec, nullptr) < 0)
    {
        qWarning() << "[ShowVideo] 打开音频解码器失败.";
        clearFFmpegResources();
        return -1;
    }

    byteBuf.clear();

    // 创建帧
    pFrame      = av_frame_alloc();
    pFrameRGB   = av_frame_alloc();
    pAudioFrame = av_frame_alloc();
    if (!pFrame || !pFrameRGB || !pAudioFrame)
    {
        qWarning() << "[ShowVideo] 帧内存分配失败.";
        clearFFmpegResources();
        return -1;
    }

    // 音频重采样设置
    out_sample_rate = AUDIO_OUT_SAMPLE_RATE;
    // 计算重采样输出缓冲大小：以解码器帧大小（或采样率作为上界）推算最大输出样本数
    int in_samples = aCodecCtx->frame_size;
    if (in_samples <= 0) in_samples = aCodecCtx->sample_rate;
    int audio_out_samples_max = (int)av_rescale_rnd(
        in_samples, out_sample_rate, aCodecCtx->sample_rate, AV_ROUND_UP);
    audio_out_buffer = (uint8_t *)av_malloc(
        av_samples_get_buffer_size(nullptr, AUDIO_OUT_CHANNELS, audio_out_samples_max, AUDIO_OUT_FORMAT, 1));
    if (!audio_out_buffer)
    {
        qWarning() << "[ShowVideo] 音频输出缓冲分配失败.";
        clearFFmpegResources();
        return -1;
    }

    // 重采样上下文（参照 resample_audio.c）
    swr_ctx = nullptr;
    int swr_ret = swr_alloc_set_opts2(&swr_ctx, &out_ch_layout, AUDIO_OUT_FORMAT, out_sample_rate,
                                      &aCodecCtx->ch_layout, aCodecCtx->sample_fmt,
                                      aCodecCtx->sample_rate, 0, nullptr);
    if (swr_ret < 0 || !swr_ctx)
    {
        QString swrErr;
        if (swr_ret < 0)
        { char errbuf[128]; av_strerror(swr_ret, errbuf, sizeof(errbuf)); swrErr = QString::fromUtf8(errbuf); }
        else
            swrErr = QStringLiteral("swr_ctx null");
        qWarning() << "[ShowVideo] 音频重采样上下文创建失败:" << swrErr;
    }
    else if (swr_init(swr_ctx) < 0)
    {
        qWarning() << "[ShowVideo] 音频重采样初始化失败.";
    }

    // === 计算视频输出尺寸并创建 sws 上下文（保持宽高比） ===
    int dstW = ui->videoLabel->size().width();
    int dstH = ui->videoLabel->size().height();
    if (dstW <= 0) dstW = 640;
    if (dstH <= 0) dstH = 480;
    QSize outSize = computeAspectRatioSize(pCodecCtx->width, pCodecCtx->height, dstW, dstH);
    int outW = outSize.width();
    int outH = outSize.height();
    rebuildSwsContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt, outW, outH);

    // 总时长(毫秒)
    m_nDurationMs = 0;
    if (pFormatCtx->duration > 0)
        m_nDurationMs = pFormatCtx->duration / 1000;
    if (m_nDurationMs <= 0 && vstream->duration > 0)
        m_nDurationMs = (qint64)(vstream->duration * av_q2d(vstream->time_base) * 1000);
    if (m_nDurationMs <= 0) m_nDurationMs = 60000;
    qDebug() << "[ShowVideo] 总时长(ms) = " << m_nDurationMs;

    m_nCurrentMs = 0;

    // 进度条范围
    ui->progressSlider->setRange(0, (int)m_nDurationMs);
    ui->progressSlider->setEnabled(true);
    ui->progressSlider->setValue(0);

    // 时间显示
    int totalMin = (int)m_nDurationMs / 60000;
    int totalSec = ((int)m_nDurationMs % 60000) / 1000;
    ui->timeLabel->setText(QString("0:00 / %1:%2").arg(totalMin).arg(totalSec, 2, 10, QChar('0')));

    m_bUserDragging = false;
    m_bSeekPending = false;
    m_nTargetMs = 0;
    m_bPaused = false;
    updatePlayPauseIcon();

    // 开始播放
    m_bPlaying = true;

    m_genCounter++;
    m_videoWorker->setContexts(pFormatCtx,
                               pCodecCtx, videoindex,
                               aCodecCtx, audioindex,
                               pCodecCtx->width, pCodecCtx->height,
                               outW, outH,
                               img_convert_ctx,
                               pFrame, pFrameRGB, pAudioFrame,
                               out_buffer,
                               swr_ctx, audio_out_buffer,
                               &m_audioMutex, &byteBuf, out_sample_rate,
                               m_genCounter);

    // 启动视频解码线程（异步）
    QMetaObject::invokeMethod(m_videoWorker, "doDecode", Qt::QueuedConnection);

    // 启动定时器（用于音频播放 + UI 更新）
    timer->start(20);

    return 0;
}

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
#include <QStatusBar>
#include <QPixmap>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <limits>

// ============================================================
// 自定义内存 IO 回调
// ============================================================
// 旧实现把整个文件一次性塞进 AVIOContext 的 buffer，且 read / seek 回调全为 NULL。
// FFmpeg 因此把该 IO 标记为不可 seek（pb->seekable == 0、avio_size() 返回 ENOSYS）：
//   - FLV / MPEG-TS / AVI 的 av_seek_frame 直接返回「Operation not permitted」；
//   - MP3 / TS 拿不到文件长度，duration 估算失败，进度条被强行设成 60 秒；
//   - 而 MP4 / MOV / MKV / WebM / ASF 恰好因为整个文件都在同一块 buffer 内、
//     FFmpeg 能做 buffer 内 seek，表现正常。
// 这就是「只有部分格式不能拖动进度条」的原因。提供 read/seek 回调后所有容器统一可 seek。
static int memIORead(void *opaque, uint8_t *buf, int bufSize)
{
    MemIOData *m = static_cast<MemIOData *>(opaque);
    if (!m || !m->data || bufSize <= 0)
        return AVERROR(EINVAL);
    qint64 remain = m->size - m->pos;
    if (remain <= 0)
        return AVERROR_EOF;
    int n = (int)qMin<qint64>(remain, bufSize);
    memcpy(buf, m->data + m->pos, (size_t)n);
    m->pos += n;
    return n;
}

static int64_t memIOSeek(void *opaque, int64_t offset, int whence)
{
    MemIOData *m = static_cast<MemIOData *>(opaque);
    if (!m)
        return AVERROR(EINVAL);
    if (whence == AVSEEK_SIZE)
        return m->size;
    int w = whence & ~AVSEEK_FORCE;
    int64_t base = 0;
    if (w == SEEK_CUR)      base = m->pos;
    else if (w == SEEK_END) base = m->size;
    else if (w != SEEK_SET) return AVERROR(EINVAL);
    int64_t newPos = base + offset;
    if (newPos < 0)
        return AVERROR(EINVAL);
    m->pos = newPos;   // 允许越过文件尾；后续 read 返回 EOF，与真实文件行为一致
    return newPos;
}

// ============================================================
// VideoWorker 实现 — 运行在独立线程，负责所有解码 + 帧调度
// ============================================================

VideoWorker::VideoWorker(QObject *parent)
    : QObject(parent), m_lastPtsMs(0)
{
}

VideoWorker::~VideoWorker()
{
    // 兜底释放：正常路径由 releaseVideoResources() 在解码线程停止后调用，
    // 这里防止异常路径（线程未退出、worker 直接被删）导致资源泄漏。
    releaseVideoResources();
}

void VideoWorker::setContexts(AVFormatContext *fmtCtx,
                              AVCodecContext *vCodecCtx, int videoIdx,
                              AVCodecContext *aCodecCtx, int audioIdx,
                              int width, int height,
                              AVPixelFormat srcPixFmt,
                              AVFrame *frame, AVFrame *audioFrame,
                              SwrContext *swr, uint8_t *aOutBuf,
                              QMutex *aMutex, QByteArray *aByteBuf,
                              int outSampleRate, int genId,
                              int audioOutBufCapacity)
{
    QMutexLocker locker(&m_mutex);
    pFormatCtx = fmtCtx;
    pCodecCtx  = vCodecCtx;
    videoindex = videoIdx;
    this->aCodecCtx  = aCodecCtx;
    audioindex = audioIdx;
    srcW = width;
    srcH = height;
    m_srcPixFmt = srcPixFmt;
    pFrame = frame;
    pAudioFrame = audioFrame;
    swr_ctx = swr;
    audio_out_buffer = aOutBuf;
    m_audioMutex = aMutex;
    m_audioByteBuf = aByteBuf;
    m_outSampleRate = outSampleRate;
    m_audioOutBufCapacity = audioOutBufCapacity;
    m_genId = genId;
    // 新文件从零开始：输出尺寸请求清空，缩放上下文与缓冲也一并作废，
    // 由主线程重新 requestOutputSize()，解码线程在下一帧前重建。
    m_reqOutSize.store(0, std::memory_order_relaxed);
    outW.store(0, std::memory_order_relaxed);
    outH.store(0, std::memory_order_relaxed);
    if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
    if (m_outBuf) { av_freep(&m_outBuf); m_outBufSize = 0; }
    for (int i = 0; i < AV_NUM_DATA_POINTERS; ++i)
    {
        m_outData[i] = nullptr;
        m_outLinesize[i] = 0;
    }
    // 新播放开始时清除 stop / pause / seek 快进标志
    m_stopRequested.store(0, std::memory_order_relaxed);
    m_paused.store(0, std::memory_order_relaxed);
    m_seekTargetAbsMs.store(0, std::memory_order_relaxed);
    m_pauseCond.wakeAll();
}

// 主线程请求新的视频输出尺寸（窗口缩放时）。
// 只做一次原子写：解码线程在下一帧前读取并在自己的线程内重建 sws 上下文。
void VideoWorker::requestOutputSize(int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    // 宽高打包进一个 64 位量：保证读到的是成对的宽高，不会出现「新宽 + 旧高」
    const quint64 packed = (quint64(quint32(h)) << 32) | quint64(quint32(w));
    m_reqOutSize.store(packed, std::memory_order_relaxed);
}

// 在解码线程内重建视频缩放上下文与 RGB 输出缓冲。
// 必须与 sws_scale 在同一线程：否则主线程释放旧缓冲时解码线程可能还在用它。
void VideoWorker::rebuildVideoSws(int srcW, int srcH, AVPixelFormat srcPixFmt,
                                  int newW, int newH)
{
    if (newW <= 0 || newH <= 0)
        return;
    if (srcW <= 0 || srcH <= 0)
        return;
    if (srcPixFmt == AV_PIX_FMT_NONE)
        return;

    if (m_sws && outW.load(std::memory_order_relaxed) == newW
                 && outH.load(std::memory_order_relaxed) == newH)
        return;   // 尺寸没变，复用现有上下文

    if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }

    const int need = av_image_get_buffer_size(AV_PIX_FMT_RGB32, newW, newH, 1);
    if (need < 0)
    {
        qWarning() << "[VideoWorker] 输出缓冲大小计算失败";
        return;
    }
    if (!m_outBuf || m_outBufSize < need)
    {
        av_freep(&m_outBuf);
        m_outBuf = (uint8_t *)av_malloc(need);
        m_outBufSize = m_outBuf ? need : 0;
    }
    if (!m_outBuf)
    {
        qWarning() << "[VideoWorker] 输出缓冲分配失败";
        return;
    }

    av_image_fill_arrays(m_outData, m_outLinesize, m_outBuf,
                         AV_PIX_FMT_RGB32, newW, newH, 1);

    m_sws = sws_getContext(srcW, srcH, srcPixFmt,
                           newW, newH, AV_PIX_FMT_RGB32,
                           SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_sws)
    {
        qWarning() << "[VideoWorker] sws_getContext 失败";
        return;
    }
    outW.store(newW, std::memory_order_relaxed);
    outH.store(newH, std::memory_order_relaxed);
}

// 释放解码线程持有的 sws 上下文与输出缓冲。
// 只能在解码线程已停止后调用。
void VideoWorker::releaseVideoResources()
{
    if (m_sws)    { sws_freeContext(m_sws); m_sws = nullptr; }
    if (m_outBuf) { av_freep(&m_outBuf); m_outBufSize = 0; }
    for (int i = 0; i < AV_NUM_DATA_POINTERS; ++i)
    {
        m_outData[i] = nullptr;
        m_outLinesize[i] = 0;
    }
    outW.store(0, std::memory_order_relaxed);
    outH.store(0, std::memory_order_relaxed);
    m_reqOutSize.store(0, std::memory_order_relaxed);
}

void VideoWorker::setSeekTarget(qint64 absMs)
{
    m_seekTargetAbsMs.store(absMs, std::memory_order_relaxed);
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

void VideoWorker::updateGenId(int genId)
{
    QMutexLocker locker(&m_mutex);
    m_genId = genId;
}

bool VideoWorker::waitForDecodingStopped(unsigned long timeoutMs)
{
    QMutexLocker locker(&m_mutex);
    QElapsedTimer elapsed;
    elapsed.start();
    // 带 predicate 重检：QWaitCondition 可能虚假唤醒，单靠 wait() 的返回值
    // 会把「还没真正退出 doDecode」误判成「已停止」。
    while (m_running.load(std::memory_order_acquire) != 0)
    {
        const qint64 left = qint64(timeoutMs) - elapsed.elapsed();
        if (left <= 0)
            return false;
        if (!m_stoppedCond.wait(&m_mutex, static_cast<unsigned long>(left)))
            return false;
    }
    return true;
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
    // 纯音频 PTS 帧调度（无视频帧时，视频分支的时钟不起作用）
    QElapsedTimer audioClock;
    bool audioClockStarted = false;
    qint64 audioFirstPtsMs = -1;
    qint64 audioPausedClockUs = 0;

    while (true)
    {
        if (m_stopRequested.load(std::memory_order_relaxed))
            goto stop_decode;

        // ---- 暂停处理：阻塞在条件变量上，直到被唤醒（继续/停止） ----
        if (m_paused.load(std::memory_order_relaxed) && !wasPausedAtLoop)
        {
            QMutexLocker locker(&m_mutex);
            qint64 pauseBegin = playClock.isValid() ? playClock.nsecsElapsed() / 1000 : 0;
            qint64 aPauseBegin = audioClock.isValid() ? audioClock.nsecsElapsed() / 1000 : 0;
            while (m_paused.load(std::memory_order_relaxed)
                   && !m_stopRequested.load(std::memory_order_relaxed))
            {
                m_pauseCond.wait(&m_mutex);
            }
            if (m_stopRequested.load(std::memory_order_relaxed))
                goto stop_decode;
            // 记下本次暂停持续的时间（视频时钟 + 音频时钟分别累计）
            qint64 pauseEnd = playClock.isValid() ? playClock.nsecsElapsed() / 1000 : 0;
            pausedClockUs += (pauseEnd - pauseBegin);
            qint64 aPauseEnd = audioClock.isValid() ? audioClock.nsecsElapsed() / 1000 : 0;
            audioPausedClockUs += (aPauseEnd - aPauseBegin);
            wasPausedAtLoop = true;
        }
        wasPausedAtLoop = false;

        if (!pFormatCtx)
            break;

        int ret = av_read_frame(pFormatCtx, pkt);
        if (ret < 0)
        {
            // 播放结束或 EOF：flush 解码器/重采样器，把内部缓存的尾帧/尾 PCM
            // 取出来；否则帧级多线程/B 帧下的最后若干帧会丢失。
            auto appendPcm = [&](const uint8_t *pcm, int nSamples) {
                const int bytes = av_samples_get_buffer_size(nullptr, AUDIO_OUT_CHANNELS,
                                                             nSamples, AUDIO_OUT_FORMAT, 1);
                if (bytes <= 0) return;
                QMutexLocker locker(m_audioMutex);
                const int buffered = m_audioByteBuf->size();
                if (buffered + bytes <= AUDIO_BUFFER_LIMIT_BYTES
                    || (bytes > AUDIO_BUFFER_LIMIT_BYTES && buffered == 0))
                    m_audioByteBuf->append(reinterpret_cast<const char *>(pcm), bytes);
            };
            auto drainVideo = [&]() {
                if (!pCodecCtx) return;
                avcodec_send_packet(pCodecCtx, nullptr);
                while (true)
                {
                    const int r = avcodec_receive_frame(pCodecCtx, pFrame);
                    if (r < 0) break;
                    if (pFrame->width > 0 && pFrame->height > 0
                        && (pFrame->width != srcW || pFrame->height != srcH
                            || pFrame->format != int(m_srcPixFmt)))
                    {
                        srcW = pFrame->width;
                        srcH = pFrame->height;
                        m_srcPixFmt = AVPixelFormat(pFrame->format);
                        rebuildVideoSws(srcW, srcH, m_srcPixFmt,
                                        outW.load(std::memory_order_relaxed),
                                        outH.load(std::memory_order_relaxed));
                    }
                    if (!m_sws || !m_outData[0]) continue;
                    sws_scale(m_sws, (const unsigned char * const *)pFrame->data,
                              pFrame->linesize, 0, srcH, m_outData, m_outLinesize);
                    QImage img((uchar *)m_outData[0],
                               outW.load(std::memory_order_relaxed),
                               outH.load(std::memory_order_relaxed),
                               QImage::Format_RGB32);
                    img = img.copy();
                    qint64 ptsMs = 0;
                    if (pFrame->pts != AV_NOPTS_VALUE && videoindex >= 0)
                    {
                        ptsMs = qint64(pFrame->pts
                                       * av_q2d(pFormatCtx->streams[videoindex]->time_base) * 1000);
                        if (ptsMs < 0) ptsMs = 0;
                    }
                    emit frameDecoded(img, ptsMs, m_genId);
                    if (ptsMs > 0) m_lastPtsMs = ptsMs;
                }
            };
            auto drainAudio = [&]() {
                if (!aCodecCtx) return;
                avcodec_send_packet(aCodecCtx, nullptr);
                while (true)
                {
                    const int r = avcodec_receive_frame(aCodecCtx, pAudioFrame);
                    if (r < 0) break;
                    if (!swr_ctx) continue;
                    int outCount = swr_get_out_samples(swr_ctx, pAudioFrame->nb_samples);
                    if (outCount <= 0 || outCount > m_audioOutBufCapacity)
                        outCount = m_audioOutBufCapacity;
                    const int len = swr_convert(swr_ctx, &audio_out_buffer, outCount,
                                                (const uint8_t **)pAudioFrame->data,
                                                pAudioFrame->nb_samples);
                    if (len > 0) appendPcm(audio_out_buffer, len);
                }
                if (swr_ctx)
                {
                    int outCount = swr_get_out_samples(swr_ctx, 0);
                    if (outCount <= 0 || outCount > m_audioOutBufCapacity)
                        outCount = m_audioOutBufCapacity;
                    const int len = swr_convert(swr_ctx, &audio_out_buffer, outCount, nullptr, 0);
                    if (len > 0) appendPcm(audio_out_buffer, len);
                }
            };
            drainVideo();
            drainAudio();
            break;
        }

        // seek 快进：AVSEEK_FLAG_BACKWARD 会定位到目标前的关键帧。
        // 目标前的包必须照常送进解码器（否则 P/B 帧没有参考帧，根本解不出画面），
        // 但解码结果不显示、不送音频输出、也不做实时帧调度，全速追上目标。
        // 用包 PTS 判断，音视频统一处理。
        bool fastForwarding = false;
        qint64 seekTarget = m_seekTargetAbsMs.load(std::memory_order_relaxed);
        if (seekTarget > 0)
        {
            if (pkt->pts == AV_NOPTS_VALUE)
            {
                // 无时间戳时无法判断，放弃快进以免永远卡住
                m_seekTargetAbsMs.store(0, std::memory_order_relaxed);
            }
            else if (pkt->stream_index >= 0
                     && pkt->stream_index < (int)pFormatCtx->nb_streams)
            {
                double secs = pkt->pts
                            * av_q2d(pFormatCtx->streams[pkt->stream_index]->time_base);
                qint64 pktMs = (qint64)(secs * 1000);
                fastForwarding = (pktMs < seekTarget);
                if (!fastForwarding)
                    m_seekTargetAbsMs.store(0, std::memory_order_relaxed);
            }
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

                    if (fastForwarding)
                    {
                        // 快进：帧已解码（为后续 P/B 帧建立参考），但不显示、不等待
                        if (m_stopRequested.load(std::memory_order_relaxed))
                            goto stop_decode;
                        continue;
                    }

                    // ---- 输出尺寸可能已被主线程改变（窗口缩放）----
                    // 解码线程在自己的线程内重建 sws 上下文与输出缓冲：
                    // 既保证新帧按新尺寸输出，也不会出现「主线程释放旧缓冲、
                    // 解码线程还在 sws_scale」的 use-after-free。
                    const quint64 reqSize = m_reqOutSize.load(std::memory_order_relaxed);
                    if (reqSize != 0)
                    {
                        const int reqW = int(reqSize & 0xffffffffu);
                        const int reqH = int(reqSize >> 32);
                        if (reqW != outW.load(std::memory_order_relaxed)
                         || reqH != outH.load(std::memory_order_relaxed))
                            rebuildVideoSws(srcW, srcH, m_srcPixFmt, reqW, reqH);
                    }

                    if (!m_sws || !m_outData[0])
                        continue;   // 缩放上下文不可用（分配失败）：跳过该帧

                    // 像素格式转换 + 直接缩放到目标尺寸（保持宽高比由 sws 上下文保证）
                    sws_scale(m_sws, (const unsigned char* const*)pFrame->data,
                              pFrame->linesize, 0, srcH,
                              m_outData, m_outLinesize);

                    // 构建 QImage（数据已为目标尺寸 outW x outH）
                    QImage img((uchar*)m_outData[0],
                               outW.load(std::memory_order_relaxed),
                               outH.load(std::memory_order_relaxed),
                               QImage::Format_RGB32);
                    img = img.copy(); // 深拷贝，安全传递到主线程

                    // 计算 PTS
                    qint64 ptsMs = 0;
                    if (pFrame->pts != AV_NOPTS_VALUE)
                    {
                        double secs = pFrame->pts * av_q2d(pFormatCtx->streams[videoindex]->time_base);
                        ptsMs = (qint64)(secs * 1000);
                        if (ptsMs < 0) ptsMs = 0;
                    }

                    emit frameDecoded(img, ptsMs, m_genId);

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

                    if (fastForwarding)
                    {
                        // 快进：音频不写入缓冲、不做调度、不更新进度
                        if (m_stopRequested.load(std::memory_order_relaxed))
                            goto stop_decode;
                        continue;
                    }

                    if (swr_ctx)
                    {
                        // 使用 swr_get_out_samples 获取含内部延迟的上限输出样本数，
                        // 并以实际缓冲区容量为上限，避免 swr_convert 写入超出缓冲区
                        int out_count = swr_get_out_samples(swr_ctx, pAudioFrame->nb_samples);
                        if (out_count <= 0 || out_count > m_audioOutBufCapacity)
                            out_count = m_audioOutBufCapacity;

                        int len = swr_convert(swr_ctx, &audio_out_buffer, out_count,
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
                    // 纯音频播放（无视频流）时，用音频 PTS 驱动进度显示与自动停止，
                    // 否则 onFrameDecoded 因无视频帧永不触发，进度无法推进。
                    if (videoindex < 0)
                    {
                        qint64 aptsMs = 0;
                        if (pAudioFrame->pts != AV_NOPTS_VALUE)
                        {
                            double secs = pAudioFrame->pts * av_q2d(pFormatCtx->streams[audioindex]->time_base);
                            aptsMs = (qint64)(secs * 1000);
                            if (aptsMs < 0) aptsMs = 0;
                        }

                        // === 纯音频帧调度：模拟视频时钟节奏，按实时速率消费音频包 ===
                        // 无视频帧时，视频分支的 playClock 不起作用，解码器会全速运行导致
                        // 音频缓冲瞬间爆满 → 后续音频被丢弃 → 播放立即结束。
                        // 此处引入独立的音频时钟，确保解码与播放同步。
                        if (!audioClockStarted)
                        {
                            audioClock.start();
                            audioFirstPtsMs = aptsMs;  // 记录首帧 PTS 作为基准
                            audioClockStarted = true;
                        }

                        if (aptsMs > 0)
                        {
                            // 当前音频播放位置 = 真实流逝时间刨去暂停期间
                            qint64 aclockUs = audioClock.nsecsElapsed() / 1000 - audioPausedClockUs;
                            qint64 aclockMs = aclockUs / 1000;
                            qint64 atargetMs = aptsMs - audioFirstPtsMs;

                            if (atargetMs > aclockMs)
                            {
                                // 提前了：等待到目标时刻（分片等待以响应 stop/pause）
                                qint64 awaitUs = (atargetMs - aclockMs) * 1000;
                                while (awaitUs > 0)
                                {
                                    if (m_stopRequested.load(std::memory_order_relaxed))
                                        goto stop_decode;
                                    if (m_paused.load(std::memory_order_relaxed))
                                        break;
                                    qint64 chunk = qMin<qint64>(awaitUs, 2000);
                                    QThread::usleep((unsigned long)chunk);
                                    awaitUs -= chunk;
                                }
                            }
                            // 若等待中检测到暂停，跳出内层循环回到外层暂停处理
                            if (m_paused.load(std::memory_order_relaxed))
                                break;
                        }

                        emit audioPtsUpdated(aptsMs, m_genId);
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
    connect(m_videoWorker, &VideoWorker::audioPtsUpdated,
            this, &MainWindow::onAudioPtsUpdated, Qt::QueuedConnection);
    connect(m_videoWorker, &VideoWorker::decodingFinished,
            this, &MainWindow::onDecodingFinished, Qt::QueuedConnection);

    m_videoThread->start();
}

// 诊断用：解码线程当前实际使用的视频输出尺寸
QSize MainWindow::decodedOutputSize() const
{
    if (!m_videoWorker)
        return QSize();
    return QSize(m_videoWorker->currentOutW(), m_videoWorker->currentOutH());
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

void MainWindow::onFrameDecoded(QImage img, qint64 ptsMs, int genId)
{
    if (!m_bPlaying || m_bPaused)
        return;

    // 过滤 seek 前的旧帧信号：只有当 genId 与当前 m_genCounter 一致时，才更新进度条和画面
    if (genId != m_genCounter)
        return;

    QPixmap temp = QPixmap::fromImage(img);
    if (!temp.isNull())
    {
        // 画面尺寸与当前目标不一致（窗口刚缩放，解码线程还没切换输出尺寸）：
        // 直接按目标尺寸缩放，避免出现画面尺寸跳变。
        if (!m_curVideoOutSize.isEmpty() && temp.size() != m_curVideoOutSize)
            temp = temp.scaled(m_curVideoOutSize, Qt::KeepAspectRatio,
                               Qt::SmoothTransformation);
        ui->videoLabel->setPixmap(temp);
    }

    if (ptsMs > 0)
    {
        // 绝对 PTS -> 相对时间轴（MPEG-TS / MP3 的 PTS 起点非 0）
        qint64 relMs = ptsMs - m_nStartMs;
        if (relMs < 0) relMs = 0;
        // AVSEEK_FLAG_BACKWARD 会先解出目标前的关键帧；在追上目标之前
        // 保持进度条在用户选中的位置，避免「拖了又弹回开头」。
        if (m_nSeekTargetMs > 0 && relMs + 500 < m_nSeekTargetMs)
            return;
        m_nSeekTargetMs = 0;
        m_nCurrentMs = qMin(relMs, m_nDurationMs);
    }
}

void MainWindow::onDecodingFinished(int genId)
{
    // worker 已退出 doDecode：此前因等待超时而挂起的资源释放现在可以安全执行。
    // 必须放在 genId 过滤之前，否则旧世代的信号会让这次清理永远不发生。
    if (m_bCleanupPending)
    {
        clearFFmpegResources(true);
        return;
    }

    // 过滤过期的信号：只有当 genId 与当前 m_genCounter 一致时，才处理
    if (genId != m_genCounter)
        return;

    // 正常播放结束（非暂停/非拖动/非seek）
    if (m_bPlaying && !m_bPaused && !m_bUserDragging && !m_bSeekPending)
    {
        m_bPlaying = false;
        m_bPaused = false;
        updatePlayPauseIcon();

        // EOF 时 byteBuf 里可能还有最后一拍 PCM 未写出（timeCallback 才消费），
        // 先排空再停 timer，避免音频尾部被截断。
        if (audioOutput && streamOut
            && audioOutput->state() != QAudio::StoppedState
            && audioOutput->state() != QAudio::SuspendedState)
        {
            QMutexLocker locker(&m_audioMutex);
            if (!byteBuf.isEmpty())
            {
                qint64 off = 0;
                while (off < byteBuf.size())
                {
                    const qint64 n = streamOut->write(byteBuf.constData() + off, byteBuf.size() - off);
                    if (n <= 0) break;
                    off += n;
                }
                if (off > 0) byteBuf.remove(0, int(off));
                if (off < 0) qWarning() << "[EOF] 尾部 PCM 写入音频设备失败";
                else if (!byteBuf.isEmpty())
                    qWarning() << "[EOF] 尾部 PCM 部分未写入，丢弃" << byteBuf.size() << "字节";
            }
        }

        if (timer->isActive())
            timer->stop();
    }
}

void MainWindow::onAudioPtsUpdated(qint64 ptsMs, int genId)
{
    // 纯音频播放：通过音频 PTS 更新进度（替代 onFrameDecoded 的视频 PTS）
    if (!m_bPlaying || m_bPaused)
        return;

    // 过滤 seek 前的旧信号
    if (genId != m_genCounter)
        return;

    if (ptsMs > 0)
    {
        // 绝对 PTS -> 相对时间轴（MPEG-TS / MP3 的 PTS 起点非 0）
        qint64 relMs = ptsMs - m_nStartMs;
        if (relMs < 0) relMs = 0;
        // AVSEEK_FLAG_BACKWARD 会先解出目标前的关键帧；在追上目标之前
        // 保持进度条在用户选中的位置，避免「拖了又弹回开头」。
        if (m_nSeekTargetMs > 0 && relMs + 500 < m_nSeekTargetMs)
            return;
        m_nSeekTargetMs = 0;
        m_nCurrentMs = qMin(relMs, m_nDurationMs);
    }
}

void MainWindow::resizeEvent(QResizeEvent *e)
{
    // 必须调用基类实现：QMainWindow::resizeEvent 负责菜单栏/工具栏/停靠窗口的重新布局
    QMainWindow::resizeEvent(e);

    // resize 事件到达时，布局可能还没按新尺寸重新计算，
    // 因此把「按新尺寸更新视频输出」推迟到事件循环的下一轮统一处理：
    // 既能拿到最终的画面区尺寸，也能把拖动窗口时的连续 resize 合并成一次。
    if (!m_resizeUpdatePending)
    {
        m_resizeUpdatePending = true;
        QTimer::singleShot(0, this, &MainWindow::updateVideoOutputSize);
    }
}

// 按当前画面区尺寸重算视频输出尺寸，并让解码线程切换到新尺寸。
// 窗口缩放后必须调用：否则视频画面会一直停留在打开文件时的尺寸（不跟随界面）。
void MainWindow::updateVideoOutputSize()
{
    m_resizeUpdatePending = false;

    if (!pCodecCtx || videoindex < 0 || !m_videoWorker)
        return;                       // 未打开文件 / 纯音频：无视频画面可调

    // 强制布局按当前窗口尺寸重新计算，保证下面读到的是最终的画面区尺寸
    if (ui->mainLayout)
        ui->mainLayout->activate();

    const QSize areaSize = ui->videoLabel->size();
    if (areaSize.width() <= 0 || areaSize.height() <= 0)
        return;

    const QSize outSize = computeAspectRatioSize(pCodecCtx->width, pCodecCtx->height,
                                                 areaSize.width(), areaSize.height());
    if (outSize.isEmpty() || outSize == m_curVideoOutSize)
        return;

    m_curVideoOutSize = outSize;

    // 1) 通知解码线程：后续帧按新尺寸输出（解码线程内部重建 sws，线程安全）
    m_videoWorker->requestOutputSize(outSize.width(), outSize.height());

    // 2) 立即把当前画面缩放到新尺寸：
    //    暂停 / 播放结束（不会再有新帧到来）时，这是唯一的更新途径；
    //    播放中则会被下一帧覆盖，避免出现「画面卡在旧尺寸」的跳变。
    const QPixmap current = ui->videoLabel->pixmap();
    if (!current.isNull())
    {
        const QPixmap scaled = current.scaled(outSize, Qt::KeepAspectRatio,
                                               Qt::SmoothTransformation);
        ui->videoLabel->setPixmap(scaled);
    }
}

bool MainWindow::stopWorkerAndWait(unsigned long timeoutMs)
{
    if (!m_videoWorker)
        return true;
    // 请求停止（DirectConnection 设置原子标志并唤醒暂停条件变量）
    QMetaObject::invokeMethod(m_videoWorker, "requestStop", Qt::DirectConnection);
    // 阻塞等待 worker 退出 doDecode（条件变量替代旧的忙等待轮询）。
    // 返回 false 表示超时、worker 可能仍在访问 FFmpeg 上下文，调用方不得立即释放。
    return m_videoWorker->waitForDecodingStopped(timeoutMs);
}

// 刷新画面区：
// 1) 必须先清掉上一个文件残留的 pixmap —— 纯音频文件（无视频流）打开后，
//    永远不会有新的视频帧到来，上一个视频的最后一帧就会一直停留在画面上；
// 2) 纯音频文件没有画面，给出提示文字，避免用户误以为加载失败。
// 注意顺序：QLabel::setPixmap() 会清除已设置的文字，setText() 也会清除 pixmap，
// 因此必须先 setPixmap(QPixmap()) 再 setText()。
void MainWindow::clearVideoPicture()
{
    ui->videoLabel->setPixmap(QPixmap());
    ui->videoLabel->setText(QString());
}

void MainWindow::refreshVideoArea()
{
    clearVideoPicture();
    if (m_bAudioOnly)
        ui->videoLabel->setText(QStringLiteral("纯音频文件，无视频画面"));
}

void MainWindow::clearContent()
{
    // 主程序在读文件失败或 0 字节时会送 (nullptr, 0)。此时必须停掉仍在运行的
    // 解码线程与音频输出，否则上一个文件会在后台继续播放/发声。
    stopPlayback();

    // clearFFmpegResources() 已把 m_bAudioOnly 复位为 false，这里统一清空画面，
    // 保证不残留上一个文件的最后一帧。
    clearVideoPicture();
    ui->progressSlider->setRange(0, 100);
    ui->progressSlider->setValue(0);
    ui->progressSlider->setEnabled(false);
    ui->timeLabel->setText("0:00 / 0:00");

    m_nCurrentMs = 0;
    m_nDurationMs = 0;
    m_nStartMs = 0;
    m_nTargetMs = 0;
    m_nSeekTargetMs = 0;
    m_bSeekPending = false;
    m_bUserDragging = false;
    updatePlayPauseIcon();
}

bool MainWindow::stopPlaybackAndWait()
{
    // 宿主返回前必须确认 worker 已退出，否则不能 unmap 归档映射。
    if (m_videoWorker && !stopWorkerAndWait(2000))
    {
        qWarning() << "[Stop] 解码线程 2s 未退出，等待 doDecode 真正返回"
                      "（不能 QThread::wait：该线程长期运行事件循环）";
        if (!m_videoWorker->waitForDecodingStopped(30000))
        {
            qWarning() << "[Stop] doDecode 30s 仍未退出，拒绝释放上下文"
                          "（宿主据此不得 unmap）";
            m_bCleanupPending = true;
            return false;
        }
    }
    clearFFmpegResources(true);
    qDebug() << "[Stop] stopPlaybackAndWait";
    return true;
}

void MainWindow::stopPlayback()
{
    stopPlaybackAndWait();
}

void MainWindow::clearFFmpegResources(bool force)
{
    // 停止视频解码线程
    const bool stopped = stopWorkerAndWait(2000);
    if (!stopped && !force)
    {
        // worker 仍在 doDecode 中，持有这些上下文的裸指针副本（VideoWorker::setContexts）；
        // 此刻释放会造成 use-after-free。挂起清理，等 decodingFinished（worker 已退出）再执行。
        m_bCleanupPending = true;
        if (timer->isActive()) timer->stop();
        if (audioOutput) audioOutput->stop();
        qWarning() << "[Stop] 解码线程未在 2s 内退出，推迟释放 FFmpeg 资源";
        return;
    }
    m_bCleanupPending = false;

    if (timer->isActive()) timer->stop();

    if (audioOutput) { delete audioOutput; audioOutput = nullptr; streamOut = nullptr; }

    if (swr_ctx)          { swr_free(&swr_ctx); swr_ctx = nullptr; }
    if (audio_out_buffer) { av_freep(&audio_out_buffer); audio_out_buffer = nullptr; }
    // 视频缩放上下文与 RGB 输出缓冲归解码线程所有：
    // 只有解码线程确实已经停止，才能安全地让它释放（否则会与正在进行的 sws_scale 竞争）。
    if (stopped && m_videoWorker)
        QMetaObject::invokeMethod(m_videoWorker, "releaseVideoResources", Qt::DirectConnection);
    av_channel_layout_uninit(&out_ch_layout);
    av_channel_layout_default(&out_ch_layout, AUDIO_OUT_CHANNELS);

    if (pFrame)      { av_frame_free(&pFrame);      pFrame      = nullptr; }
    if (pAudioFrame) { av_frame_free(&pAudioFrame); pAudioFrame = nullptr; }
    if (pCodecCtx)  { avcodec_free_context(&pCodecCtx);  pCodecCtx = nullptr; }
    if (aCodecCtx)  { avcodec_free_context(&aCodecCtx);  aCodecCtx = nullptr; }
    if (pFormatCtx) { avformat_close_input(&pFormatCtx); pFormatCtx = nullptr; }
    // avio_context_free 会释放 AVIOContext 当前使用的内部 buffer（m_avioInternalBuffer
    // 可能被 FFmpeg 替换过），因此这里只置空指针，不能单独再 free 一次。
    if (m_avioCtx)  { avio_context_free(&m_avioCtx); m_avioCtx = nullptr; }
    m_avioInternalBuffer = nullptr;
    // 数据源与 AVIO 内部缓冲已分离：完整文件数据需要单独释放
    if (m_avioBuffer) { av_freep(&m_avioBuffer); m_avioBuffer = nullptr; }
    m_memIO.data = nullptr;
    m_memIO.size = 0;
    m_memIO.pos  = 0;

    videoindex = -1;
    audioindex = -1;
    m_bPlaying = false;
    m_bPaused = false;
    m_bAudioOnly = false;  // 重置：下次播放时重新检测
    m_curVideoOutSize = QSize();   // 重置：下次播放时按新的画面区尺寸重算

    {
        QMutexLocker locker(&m_audioMutex);
        byteBuf.clear();
    }
}

MainWindow::~MainWindow()
{
    if (timer->isActive()) timer->stop();

    // 1) 先请求 worker 退出 doDecode，确认它不再访问 FFmpeg 上下文后才能释放资源。
    bool safelyStopped = true;
    if (m_videoWorker)
    {
        QMetaObject::invokeMethod(m_videoWorker, "requestStop", Qt::DirectConnection);
        safelyStopped = m_videoWorker->waitForDecodingStopped(3000);
    }

    if (!safelyStopped)
    {
        // worker 卡住且仍持有上下文的裸指针副本：释放资源或 delete worker 都会 UAF。
        // 进程即将结束，宁可泄漏也不崩，交由操作系统回收。
        qWarning() << "[Media] 解码线程未在 3s 内退出，跳过资源释放与 worker 删除";
        delete ui;
        return;
    }

    // force=true：析构后不再有后续播放，正常路径必须释放干净
    clearFFmpegResources(true);

    // 2) 结束 QThread 事件循环
    if (m_videoThread && m_videoThread->isRunning())
    {
        m_videoThread->quit();
        if (!m_videoThread->wait(3000))
        {
            qWarning() << "[Media] QThread 未退出，跳过 worker 删除";
            m_videoWorker = nullptr;
        }
    }

    // 3) worker 无 parent，线程已静止，显式回收；
    //    m_videoThread 有 parent(this)，由 QObject 析构自动删除。
    if (m_videoWorker)
    {
        delete m_videoWorker;
        m_videoWorker = nullptr;
    }
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
// 注意：这里的 setPaused 必须用 Qt::DirectConnection，不能用 QueuedConnection。
// 原因：VideoWorker 位于 m_videoThread，而 doDecode() 是通过 QueuedConnection 进入的；
// doDecode() 是一个直到播放结束/停止才返回的长循环，整个播放期间都占着该线程的事件循环，
// 因此任何 QueuedConnection 的槽调用都会被排进队列却永远得不到执行 ——
// 表现为「暂停按钮点了没反应，视频照播」（音频因 audioOutput->suspend() 在主线程直接生效而看起来正常）。
// setPaused() 内部只做「原子标志写入 + 条件变量唤醒」且自身加锁，从主线程直接调用是线程安全的，
// 与本文件其余控制调用（requestStop / clearStopRequest / updateGenId / seek 里的 setPaused）保持一致。
void MainWindow::on_playPauseBtn_clicked(void)
{
    // Stop 或自然播放结束后 m_bPlaying=false：从起点重新播放（按钮此前是死控件）
    if (!m_bPlaying)
    {
        if (pFormatCtx == nullptr)
            return;   // 没有打开过媒体或资源已释放，无从重播
        if (timer->isActive()) timer->stop();
        doSeek(0);
        return;
    }

    m_bPaused = !m_bPaused;
    updatePlayPauseIcon();

    if (m_bPaused)
    {
        // 暂停：暂停 worker 帧调度 + 停止定时器 + 暂停音频
        QMetaObject::invokeMethod(m_videoWorker, "setPaused", Qt::DirectConnection, Q_ARG(bool, true));
        if (timer->isActive()) timer->stop();
        if (audioOutput) audioOutput->suspend();
        qDebug() << "[Play/Pause] Paused";
    }
    else
    {
        // 继续播放（同样必须 DirectConnection：暂停时 worker 正阻塞在条件变量上，
        // 事件循环依然被 doDecode 占着，队列投递无法唤醒它 → 会「暂停后无法恢复」）
        QMetaObject::invokeMethod(m_videoWorker, "setPaused", Qt::DirectConnection, Q_ARG(bool, false));
        if (!timer->isActive()) timer->start(20);
        ensureAudioStarted();   // 若曾被 Stop 过，此时音频流已失效，需要重新 start()
        qDebug() << "[Play/Pause] Playing";
    }
}

void MainWindow::updatePlayPauseIcon()
{
    // 三态：未播放/暂停显示 ▶，播放中显示 ⏸；Stop/EOF 后按钮应回到可重播状态
    if (!m_bPlaying || m_bPaused)
        ui->playPauseBtn->setText("▶");
    else
        ui->playPauseBtn->setText("⏸");
}

bool MainWindow::ensureAudioStarted()
{
    if (audioOutput == nullptr)
        return true;   // 无音频流（纯视频）

    if (audioOutput->state() == QAudio::StoppedState)
    {
        audioOutput->setVolume(m_nVolume / 100.0f);
        streamOut = audioOutput->start();
        return streamOut != nullptr;
    }
    if (audioOutput->state() == QAudio::SuspendedState)
    {
        audioOutput->resume();
        return true;
    }
    return streamOut != nullptr;
}

// ====== 停止 ======
void MainWindow::on_stopBtn_clicked(void)
{
    if (timer->isActive()) timer->stop();
    m_bPlaying = false;
    m_bPaused = false;
    updatePlayPauseIcon();

    // 清空视频画面（纯音频文件则显示提示文字）
    refreshVideoArea();

    // ====== 先停止 worker，再操作 FFmpeg 上下文，避免数据竞争导致崩溃 ======
    const bool workerStopped = stopWorkerAndWait(2000);
    if (!workerStopped)
        qWarning() << "[Stop] 解码线程未在 2s 内退出，跳过 FFmpeg 上下文重置";

    // 重置进度（worker 已停止，此时操作上下文安全）
    if (workerStopped && pFormatCtx)
    {
        av_seek_frame(pFormatCtx, -1, 0, AVSEEK_FLAG_BACKWARD);
        if (pCodecCtx) avcodec_flush_buffers(pCodecCtx);
        if (aCodecCtx) avcodec_flush_buffers(aCodecCtx);
        QMutexLocker locker(&m_audioMutex);
        byteBuf.clear();
    }

    m_nCurrentMs = 0;
    m_nSeekTargetMs = 0;
    m_nTargetMs = 0;
    m_bSeekPending = false;
    m_videoWorker->setSeekTarget(0);
    ui->progressSlider->blockSignals(true);
    ui->progressSlider->setValue(0);
    ui->progressSlider->blockSignals(false);
    ui->timeLabel->setText(QString("0:00 / %1:%2")
        .arg((int)m_nDurationMs / 60000)
        .arg(((int)m_nDurationMs % 60000) / 1000, 2, 10, QChar('0')));

    if (audioOutput) audioOutput->stop();

    // 只有 worker 真正停下来了才清除 stop 标志；超时仍放行会让旧线程
    // 在“已停止”的 UI 状态下继续解码，并与后续 seek/释放竞争。
    if (m_videoWorker && workerStopped)
    {
        QMetaObject::invokeMethod(m_videoWorker, "clearStopRequest", Qt::DirectConnection);
        QMetaObject::invokeMethod(m_videoWorker, "setPaused", Qt::DirectConnection, Q_ARG(bool, false));
    }
    else if (!workerStopped)
    {
        // 保留 stop 请求，等 worker 自己退出；不要清上下文/置 pending，
        // 否则同一文件之后无法重播。
        qWarning() << "[Stop] worker 未在 2s 内退出，保留上下文等待其退出";
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
    if (!pFormatCtx)
    {
        m_bPlaying = false;
        return;
    }
    // 纯音频文件：videoindex < 0，但 seek 仍然有效
    if (ms < 0) ms = 0;
    if (ms > m_nDurationMs) ms = m_nDurationMs;

    const qint64 previousMs = m_nCurrentMs;
    const bool   wasPlaying = m_bPlaying;

    m_bPlaying = false;
    m_bPaused = false;
    updatePlayPauseIcon();
    timer->stop();

    // 暂停视频解码线程（避免 seek 期间继续读帧）
    if (!stopWorkerAndWait(2000))
    {
        qWarning() << "[Seek] 解码线程未在 2s 内退出，放弃本次跳转";
        m_bSeekPending = false;
        m_nCurrentMs = previousMs;
        m_bPlaying = wasPlaying;   // 恢复 seek 前的播放状态，UI 不留在“已暂停”
        if (m_videoWorker)
        {
            QMetaObject::invokeMethod(m_videoWorker, "clearStopRequest", Qt::DirectConnection);
            QMetaObject::invokeMethod(m_videoWorker, "setPaused", Qt::DirectConnection, Q_ARG(bool, false));
        }
        updatePlayPauseIcon();
        if (wasPlaying && timer) timer->start(20);
        return;
    }

    // stream_index = -1 时，av_seek_frame 期望时间戳单位为 AV_TIME_BASE (微秒)；
    // UI 是相对时间轴，容器 PTS 起点非 0 时要补上起始偏移。
    int64_t target_ts_us = (ms + m_nStartMs) * 1000;
    qDebug() << "[Seek] doSeek to" << ms << "ms (ts=" << target_ts_us << "us)";

    // 优先回退到关键帧避免花屏；不支持的容器依次降级，尽量保证跳转可用
    int seek_ret = av_seek_frame(pFormatCtx, -1, target_ts_us, AVSEEK_FLAG_BACKWARD);
    if (seek_ret < 0)
        seek_ret = av_seek_frame(pFormatCtx, -1, target_ts_us, AVSEEK_FLAG_ANY);
    if (seek_ret < 0)
        seek_ret = avformat_seek_file(pFormatCtx, -1,
                                      std::numeric_limits<int64_t>::min(),
                                      target_ts_us,
                                      std::numeric_limits<int64_t>::max(), 0);

    // 重启解码线程（成功与失败两条路径共用）
    auto restartDecode = [this](int genId)
    {
        // 递增播放世代，使 seek 前的旧帧信号被 onFrameDecoded / onAudioPtsUpdated 过滤掉
        QMetaObject::invokeMethod(m_videoWorker, "updateGenId", Qt::DirectConnection, Q_ARG(int, genId));
        QMetaObject::invokeMethod(m_videoWorker, "clearStopRequest", Qt::DirectConnection);
        QMetaObject::invokeMethod(m_videoWorker, "setPaused", Qt::DirectConnection, Q_ARG(bool, false));
        QMetaObject::invokeMethod(m_videoWorker, "doDecode", Qt::QueuedConnection);
        timer->start(20);
    };

    if (seek_ret < 0)
    {
        char errbuf[128];
        av_strerror(seek_ret, errbuf, sizeof(errbuf));
        qWarning() << "[Seek] Seek failed at" << ms << "ms:" << errbuf;

        // 不再假装成功。seek 失败时 IO 位置往往已被推到文件尾（FLV/TS 实测如此），
        // 先试着回到拖动前的位置，再不行就回到开头，避免恢复播放后立刻 EOF 结束。
        int64_t backTs = (previousMs + m_nStartMs) * 1000;
        if (av_seek_frame(pFormatCtx, -1, backTs, AVSEEK_FLAG_BACKWARD) < 0)
            av_seek_frame(pFormatCtx, -1, 0, AVSEEK_FLAG_BACKWARD);

        if (pCodecCtx) avcodec_flush_buffers(pCodecCtx);
        if (aCodecCtx) avcodec_flush_buffers(aCodecCtx);
        {
            QMutexLocker locker(&m_audioMutex);
            byteBuf.clear();
        }

        m_nCurrentMs = previousMs;
        m_nSeekTargetMs = 0;
        // 清掉可能残留的快进标志，避免恢复播放后继续跳过输出
        m_videoWorker->setSeekTarget(0);
        m_bPlaying = wasPlaying;
        m_bPaused = false;
        updatePlayPauseIcon();
        ensureAudioStarted();   // Stop 过之后音频流已失效，不能只 resume()

        m_genCounter++;
        restartDecode(m_genCounter);
        statusBar()->showMessage(QStringLiteral("该媒体不支持拖动定位，已恢复原位播放"), 3000);
        return;
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
    // 关键帧回退期间保护进度条，不让更早的帧 PTS 把位置拉回去
    m_nSeekTargetMs = ms;
    m_bPlaying = true;

    // 让解码线程进入快进：目标前的包照常解码但不显示，快速追上选中位置。
    // 直接调用即可：setSeekTarget 只做一次原子写，无需经元对象字符串调用
    // （qint64 的元类型名在 Qt5/Qt6 间并不一致，字符串调用可能静默失败）。
    qDebug() << "[Seek] fast-forward target(abs ms) =" << (ms + m_nStartMs);
    m_videoWorker->setSeekTarget(ms + m_nStartMs);

    updatePlayPauseIcon();
    ensureAudioStarted();   // 首次播放/Stop 后重播：StoppedState 需要重新 start()

    m_genCounter++;
    restartDecode(m_genCounter);
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

// 大文件路径通道入口：设置一个待打开路径，再复用 showVideo 的通用流程（P1-2）
int MainWindow::showVideoFile(const QString &sPath)
{
    m_sPendingPath = sPath;
    return showVideo(nullptr, 0);
}

// v4：宿主内存映射入口。直接复用 showVideo 的通用流程，仅标记数据来自外部映射，
// 播放期间宿主必须保持映射有效（换文件/stopPlayback 后由宿主释放）。
int MainWindow::showVideoMapped(char *szFileData, qint64 nFileLen)
{
    if (szFileData == nullptr || nFileLen <= 0)
        return -1;
    m_bMappedInput = true;
    const int nRet = showVideo(szFileData, nFileLen);
    m_bMappedInput = false;
    return nRet;
}

int MainWindow::showVideo(char *szFileData, qint64 nFileLen)
{
    const QString byPath = m_sPendingPath;
    m_sPendingPath.clear();
    if (byPath.isEmpty() && (szFileData == nullptr || nFileLen <= 0))
        return -1;

    if (timer->isActive()) timer->stop();

    // 提前递增播放世代：本次打开之前排队在 UI 线程事件队列里的旧帧信号
    // （上一文件的 frameDecoded）必须被 onFrameDecoded 的世代校验拦掉，
    // 否则它们会在新文件解码出第一帧之前把旧画面重新画上去。
    ++m_genCounter;

    // 清理上次播放的资源（包含停止视频解码线程）
    clearFFmpegResources();
    if (m_bCleanupPending)
    {
        // 上一次的解码线程尚未退出，FFmpeg 上下文仍被 worker 引用；
        // 此刻不能覆盖指针，否则既泄漏又可能 UAF。等 decodingFinished 补做清理。
        qWarning() << "[ShowVideo] 上一个解码线程仍在停止中，暂不打开新文件";
        return -1;
    }

    // 立即清空画面区：切换文件时上一文件的最后一帧必须消失。
    // 若新文件没有视频流（如 WAV），之后永远不会有视频帧到来覆盖它，
    // 残留的旧画面就会一直停留（表现为「播放 WAV 时画面还是上一个 MP4」）。
    clearVideoPicture();

    av_channel_layout_default(&out_ch_layout, AUDIO_OUT_CHANNELS);

    pFormatCtx = avformat_alloc_context();
    if (!pFormatCtx)
    {
        qWarning() << "[ShowVideo] avformat_alloc_context 失败.";
        clearFFmpegResources();
        return -1;
    }

    if (byPath.isEmpty())
    {
        // —— 内存播放：整个文件由主程序读入后交给插件 ——
        if (m_bMappedInput)
        {
            // v4 内存映射通道：宿主已把归档内容区映射为只读内存，直接作为
            // AVIO 数据源，不再 av_malloc + memcpy 复制一整份（大视频的关键）。
            m_memIO.data = reinterpret_cast<unsigned char *>(szFileData);
            m_memIO.size = nFileLen;
            m_memIO.pos  = 0;
        }
        else
        {
            // 将传入的数据拷贝为独立的内存数据源
            m_avioBuffer = (unsigned char *)av_malloc(nFileLen);
            if (!m_avioBuffer)
            {
                qWarning() << "[ShowVideo] AVIO buffer allocation failed.";
                clearFFmpegResources();
                return -1;
            }
            memcpy(m_avioBuffer, szFileData, nFileLen);
            m_memIO.data = m_avioBuffer;
            m_memIO.size = nFileLen;
            m_memIO.pos  = 0;
        }

        // AVIOContext 的内部读缓冲与数据源分离，并挂上 read / seek 回调。
        // 有了 seek 回调，pb->seekable = AVIO_SEEKABLE_NORMAL 且 avio_size() 可用，
        // av_seek_frame 与容器时长估算才能对所有格式正常工作。
        const int avioBufSize = 64 * 1024;
        m_avioInternalBuffer = (unsigned char *)av_malloc(avioBufSize);
        if (!m_avioInternalBuffer)
        {
            qWarning() << "[ShowVideo] AVIO read buffer allocation failed.";
            clearFFmpegResources();
            return -1;
        }
        m_avioCtx = avio_alloc_context(m_avioInternalBuffer, avioBufSize, 0,
                                       &m_memIO, &memIORead, nullptr, &memIOSeek);
        if (!m_avioCtx)
        {
            qWarning() << "[ShowVideo] AVIO context allocation failed.";
            // 创建失败时 AVIOContext 尚未接管 buffer，需要自行释放
            av_freep(&m_avioInternalBuffer);
            clearFFmpegResources();
            return -1;
        }
        m_avioCtx->seekable = AVIO_SEEKABLE_NORMAL;
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
    }
    else
    {
        // —— 大文件路径通道：FFmpeg 直接按文件流式读取，无需把数 GB 内容读入内存 ——
        const QByteArray pathUtf8 = byPath.toUtf8();
        if (avformat_open_input(&pFormatCtx, pathUtf8.constData(), nullptr, nullptr) != 0)
        {
            qWarning() << "[ShowVideo] 无法打开输入文件:" << byPath;
            clearFFmpegResources();
            return -1;
        }
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

    // 视频与音频至少要有一种流才能播放（支持纯视频 / 纯音频 / 音视频都有）
    if (videoindex < 0 && audioindex < 0)
    {
        qWarning() << "[ShowVideo] 找不到视频或音频流.";
        clearFFmpegResources();
        return -1;
    }
    m_bAudioOnly = (videoindex < 0);

    // 流信息确定后再次刷新画面区：纯音频文件没有视频流，画面区应显示提示文字，
    // 而不是空白（更不能残留上一个视频文件的最后一帧）。
    refreshVideoArea();

    // 音频输出设置（仅在有音频流时创建，无声视频不需要）
    if (audioindex >= 0)
    {
        QAudioFormat fmt;
        fmt.setSampleRate(AUDIO_OUT_SAMPLE_RATE);
        fmt.setChannelCount(AUDIO_OUT_CHANNELS);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        // Qt6：QAudioSink 取代 Qt5 的 QAudioOutput，采样格式改用 setSampleFormat
        fmt.setSampleFormat(QAudioFormat::Int16);
#else
        fmt.setSampleSize(16);
        fmt.setCodec("audio/pcm");
        fmt.setByteOrder(QAudioFormat::LittleEndian);
        fmt.setSampleType(QAudioFormat::SignedInt);
#endif
        audioOutput = new AudioOutputDevice(fmt);
        audioOutput->setVolume(m_nVolume / 100.0f);
        streamOut = audioOutput->start();
    }

    // ========== 视频解码（仅当存在视频流时）==========
    if (videoindex >= 0)
    {
        pCodecCtx = avcodec_alloc_context3(nullptr);
        if (!pCodecCtx || avcodec_parameters_to_context(pCodecCtx, vstream->codecpar) < 0)
        {
            qWarning() << "[ShowVideo] 视频解码器上下文初始化失败.";
            clearFFmpegResources();
            return -1;
        }

        // 解码包的时间基必须与所属流一致。libavcodec 用 pkt_timebase 把
        // skip_samples / discard_padding（MP3 LAME/Xing、AAC priming、Opus pre-skip
        // 等 gapless 信息）换算成时间戳来修正 frame->pts / duration。
        // 未设置时 pkt_timebase.num == 0，FFmpeg 只能打印
        // "Could not update timestamps for skipped/discarded samples" 并放弃修正，
        // 导致音频帧 PTS 偏早约一个 encoder delay（mp3 约 25ms）。
        pCodecCtx->pkt_timebase = vstream->time_base;

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
    }

    // ========== 音频解码（仅当存在音频流时）==========
    if (audioindex >= 0 && astream)
    {
        aCodecCtx = avcodec_alloc_context3(nullptr);
        if (!aCodecCtx || avcodec_parameters_to_context(aCodecCtx, astream->codecpar) < 0)
        {
            qWarning() << "[ShowVideo] 音频解码器上下文初始化失败.";
            clearFFmpegResources();
            return -1;
        }

        // 同视频解码器：不设置 pkt_timebase 会让 gapless 跳过样本无法修正 PTS
        aCodecCtx->pkt_timebase = astream->time_base;

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
    }

    byteBuf.clear();

    // 创建帧（视频帧仅在有视频流时分配）
    if (videoindex >= 0)
    {
        pFrame = av_frame_alloc();
        if (!pFrame)
        {
            qWarning() << "[ShowVideo] 视频帧内存分配失败.";
            clearFFmpegResources();
            return -1;
        }
    }
    pAudioFrame = av_frame_alloc();
    if (!pAudioFrame)
    {
        qWarning() << "[ShowVideo] 音频帧内存分配失败.";
        clearFFmpegResources();
        return -1;
    }

    // 音频重采样设置（仅在有音频流时）
    out_sample_rate = AUDIO_OUT_SAMPLE_RATE;
    int audio_out_samples_max = 0;  // 传递给 worker 作为音频缓冲容量上限；无声视频时为 0

    if (audioindex >= 0 && aCodecCtx)
    {
        // 1. 先创建并初始化重采样上下文（参照 resample_audio.c）
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
            swr_free(&swr_ctx);
            swr_ctx = nullptr;
        }

        // 2. 计算重采样输出缓冲大小
        //    使用 swr_get_out_samples() 获取含内部滤波器延迟的实际所需缓冲区大小，
        //    避免 swr_convert 写入超出缓冲区（特别是采样率不同时的重采样场景）。
        int in_samples = aCodecCtx->frame_size;
        if (in_samples <= 0) in_samples = aCodecCtx->sample_rate;
        if (swr_ctx)
        {
            audio_out_samples_max = swr_get_out_samples(swr_ctx, in_samples);
            if (audio_out_samples_max <= 0)
            {
                // swr_get_out_samples 返回非正值时，回退到按比例估算
                audio_out_samples_max = (int)av_rescale_rnd(
                    in_samples, out_sample_rate, aCodecCtx->sample_rate, AV_ROUND_UP);
            }
        }
        else
        {
            // swr_ctx 不可用时，按比例估算
            audio_out_samples_max = (int)av_rescale_rnd(
                in_samples, out_sample_rate, aCodecCtx->sample_rate, AV_ROUND_UP);
        }
        // 额外预留 64 个样本空间以应对边界情况
        audio_out_samples_max += 64;

        audio_out_buffer = (uint8_t *)av_malloc(
            av_samples_get_buffer_size(nullptr, AUDIO_OUT_CHANNELS, audio_out_samples_max, AUDIO_OUT_FORMAT, 1));
        if (!audio_out_buffer)
        {
            qWarning() << "[ShowVideo] 音频输出缓冲分配失败.";
            clearFFmpegResources();
            return -1;
        }
    }

    // === 计算视频输出尺寸（保持宽高比，仅视频文件） ===
    // 注意：sws 上下文由解码线程自己创建 —— 窗口缩放时必须在解码线程内重建，
    // 主线程不能释放正在被 sws_scale 使用的缓冲。这里只算出目标尺寸。
    int outW = 0, outH = 0;
    if (videoindex >= 0)
    {
        int dstW = ui->videoLabel->size().width();
        int dstH = ui->videoLabel->size().height();
        if (dstW <= 0) dstW = 640;
        if (dstH <= 0) dstH = 480;
        QSize outSize = computeAspectRatioSize(pCodecCtx->width, pCodecCtx->height, dstW, dstH);
        outW = outSize.width();
        outH = outSize.height();
    }

    // 总时长(毫秒)：容器 -> 流 -> 按比特率/文件大小估算 -> 兜底
    m_nDurationMs = 0;
    if (pFormatCtx->duration > 0)
        m_nDurationMs = pFormatCtx->duration / 1000;
    AVStream *durStream = (videoindex >= 0) ? vstream : astream;
    if (m_nDurationMs <= 0 && durStream && durStream->duration > 0)
        m_nDurationMs = (qint64)(durStream->duration * av_q2d(durStream->time_base) * 1000);
    if (m_nDurationMs <= 0 && pFormatCtx->bit_rate > 0)
    {
        // 按路径打开时 FFmpeg 自建的是 pFormatCtx->pb，m_avioCtx 为 nullptr；
        // 内存通道时 pFormatCtx->pb 就是 m_avioCtx。统一用 pb 两种通道都有效，
        // 否则大文件按路径打开且容器无时长时会退化为兜底的 60000ms。
        int64_t fileSize = avio_size(pFormatCtx->pb);
        if (fileSize > 0)
            m_nDurationMs = (qint64)(fileSize * 8.0 * 1000.0 / pFormatCtx->bit_rate);
    }
    if (m_nDurationMs <= 0) m_nDurationMs = 60000;
    qDebug() << "[ShowVideo] 总时长(ms) = " << m_nDurationMs;

    // 部分容器（MPEG-TS / MP3）的 PTS 起点非 0：进度显示要减去该偏移，
    // seek 目标要加上该偏移，才能与 UI 的相对时间轴对齐。
    m_nStartMs = 0;
    if (pFormatCtx->start_time != AV_NOPTS_VALUE && pFormatCtx->start_time > 0)
        m_nStartMs = pFormatCtx->start_time / 1000;
    qDebug() << "[ShowVideo] 起始偏移(ms) = " << m_nStartMs;

    m_nCurrentMs = 0;
    m_nSeekTargetMs = 0;

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

    // 开始播放（先置 m_bPlaying，再刷新图标：图标三态依赖它）
    m_bPlaying = true;
    updatePlayPauseIcon();

    // 播放世代已在函数开头递增过（用于让上一文件排队中的帧信号失效），
    // 此处直接把当前世代交给 worker，使新帧的 genId 与 m_genCounter 一致。
    m_videoWorker->setContexts(pFormatCtx,
                               pCodecCtx, videoindex,
                               aCodecCtx, audioindex,
                               (videoindex >= 0) ? pCodecCtx->width  : 0,
                               (videoindex >= 0) ? pCodecCtx->height : 0,
                               (videoindex >= 0) ? pCodecCtx->pix_fmt : AV_PIX_FMT_NONE,
                               pFrame, pAudioFrame,
                               swr_ctx, audio_out_buffer,
                               &m_audioMutex, &byteBuf, out_sample_rate,
                               m_genCounter,
                               audio_out_samples_max);

    // 请求解码线程按目标尺寸输出（setContexts 已清空旧请求，必须在其之后调用）。
    // 之后窗口缩放时由 updateVideoOutputSize() 再次请求新尺寸。
    if (videoindex >= 0)
    {
        m_curVideoOutSize = QSize(outW, outH);
        m_videoWorker->requestOutputSize(outW, outH);
    }

    // 启动视频解码线程（异步）
    QMetaObject::invokeMethod(m_videoWorker, "doDecode", Qt::QueuedConnection);

    // 启动定时器（用于音频播放 + UI 更新）
    timer->start(20);

    return 0;
}

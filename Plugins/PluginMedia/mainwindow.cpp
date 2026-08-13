#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QFile>
#include <QDebug>
#include <QTimer>
#include <QSlider>
#include <QDateTime>
#include <QCoreApplication>

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    audioOutput(nullptr),
    streamOut(nullptr),
    swr_ctx(nullptr),
    audio_out_buffer(nullptr),
    img_convert_ctx(nullptr),
    packet(nullptr),
    out_buffer(nullptr),
    m_bPaused(false),
    m_nVolume(80)
{
    ui->setupUi(this);

    // 初始化音频输出(nullptr), 初始化音频通道布局
    av_channel_layout_default(&out_ch_layout, 2);

    ui->progressSlider->setRange(0, 100);
    ui->progressSlider->setEnabled(false);

    // 设置播放/暂停按钮初始状态为播放
    ui->playPauseBtn->setText("⏸");

    timer = new QTimer(this);
    timer->setTimerType(Qt::PreciseTimer);
    connect(timer, SIGNAL(timeout()), this, SLOT(timeCallback()));
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

    // 2. 逐帧解码（仅当正在播放且非暂停且非用户拖动时）
    if (m_bPlaying && !m_bPaused && !m_bUserDragging)
    {
        decodeOneFrame(packet);
    }

    // 3. 音频缓存播放
    if (audioOutput && audioOutput->state() != QAudio::StoppedState
     && audioOutput->state() != QAudio::SuspendedState)
    {
        int writeBytes = qMin(byteBuf.length(), audioOutput->bytesFree());
        streamOut->write(byteBuf.data(), writeBytes);
        byteBuf = byteBuf.right(byteBuf.length() - writeBytes);
    }

    // 4. 最后更新 UI 显示（避免覆盖 seek 后的位置）
    updateProgressDisplay();
}

void Delay_MSec(unsigned int msec)
{
    QTime _Timer = QTime::currentTime().addMSecs(msec);
    while (QTime::currentTime() < _Timer)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
}

void MainWindow::resizeEvent(QResizeEvent*)
{
    videoW = ui->videoLabel->size().width();
    videoH = ui->videoLabel->size().height();
}

void MainWindow::clearFFmpegResources()
{
    if (audioOutput) { delete audioOutput; audioOutput = nullptr; streamOut = nullptr; }

    if (swr_ctx)          { swr_free(&swr_ctx); swr_ctx = nullptr; }
    if (audio_out_buffer) { av_freep(&audio_out_buffer); audio_out_buffer = nullptr; }
    if (img_convert_ctx)  { sws_freeContext(img_convert_ctx); img_convert_ctx = nullptr; }
    if (out_buffer)       { av_freep(&out_buffer); out_buffer = nullptr; }
    if (packet)           { av_packet_free(&packet); packet = nullptr; }
    av_channel_layout_uninit(&out_ch_layout);
    av_channel_layout_default(&out_ch_layout, 2);

    if (pFrameRGB)  { av_frame_free(&pFrameRGB);  pFrameRGB  = nullptr; }
    if (pFrame)     { av_frame_free(&pFrame);     pFrame     = nullptr; }
    if (pCodecCtx)  { avcodec_free_context(&pCodecCtx);  pCodecCtx = nullptr; }
    if (aCodecCtx)  { avcodec_free_context(&aCodecCtx);  aCodecCtx = nullptr; }
    if (pFormatCtx) { avformat_close_input(&pFormatCtx); pFormatCtx = nullptr; }
    videoindex = -1;
    audioindex = -1;
    m_bPlaying = false;
    m_bPaused = false;
}

MainWindow::~MainWindow()
{
    if (timer->isActive()) timer->stop();
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
    timer->start(frameRate);
}

// ====== 播放/暂停 ======
void MainWindow::on_playPauseBtn_clicked(void)
{
    if (!m_bPlaying) return;

    m_bPaused = !m_bPaused;
    updatePlayPauseIcon();

    if (m_bPaused)
    {
        // 暂停：停止定时器
        if (timer->isActive()) timer->stop();
        if (audioOutput) audioOutput->suspend();
        qDebug() << "[Play/Pause] Paused";
    }
    else
    {
        // 继续播放
        if (!timer->isActive()) timer->start(frameRate);
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

    // 重置进度
    if (pFormatCtx)
    {
        av_seek_frame(pFormatCtx, -1, 0, AVSEEK_FLAG_BACKWARD);
        if (pCodecCtx) avcodec_flush_buffers(pCodecCtx);
        if (aCodecCtx) avcodec_flush_buffers(aCodecCtx);
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

    // stream_index = -1 时，av_seek_frame 期望时间戳单位为 AV_TIME_BASE (微秒)
    int64_t target_ts_us = ms * 1000;
    qDebug() << "[Seek] doSeek to" << ms << "ms (ts=" << target_ts_us << "us)";

    // 执行 seek（回退到关键帧，避免花屏）
    int seek_ret = av_seek_frame(pFormatCtx, -1, target_ts_us, AVSEEK_FLAG_BACKWARD);
    if (seek_ret < 0)
    {
        qWarning() << "Seek failed at" << ms << "ms, ret=" << seek_ret;
    }

    // 清理解码器内部缓冲，从关键帧开始重新解码
    if (pCodecCtx) avcodec_flush_buffers(pCodecCtx);
    if (aCodecCtx) avcodec_flush_buffers(aCodecCtx);

    // 清空音频缓存（音画同步）
    byteBuf.clear();

    m_nCurrentMs = ms;
    m_bPlaying = true;

    if (audioOutput) audioOutput->resume();

    if (!timer->isActive())
        timer->start(frameRate);
}

// ====== 解码并显示一帧 ======
bool MainWindow::decodeOneFrame(AVPacket *packet)
{
    if (!pFormatCtx) return false;

    int ret = av_read_frame(pFormatCtx, packet);
    if (ret < 0)
    {
        // 播放结束
        m_bPlaying = false;
        m_bPaused = false;
        updatePlayPauseIcon();
        if (timer->isActive()) timer->stop();
        return false;
    }

    if (packet->stream_index == audioindex)
    {
        // 音频解码
        ret = avcodec_send_packet(aCodecCtx, packet);
        if (ret >= 0)
        {
            while (ret >= 0)
            {
                ret = avcodec_receive_frame(aCodecCtx, pFrame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0)
                {
                    qWarning() << "音频解码失败.";
                    break;
                }

                if (swr_ctx)
                {
                    const AVSampleFormat out_fmt = AV_SAMPLE_FMT_S16;
                    int len = swr_convert(swr_ctx, &audio_out_buffer, MAX_AUDIO_FRAME_SIZE,
                                          (const uint8_t **)pFrame->data, pFrame->nb_samples);
                    if (len > 0)
                    {
                        int dst_bufsize = av_samples_get_buffer_size(nullptr, out_ch_layout.nb_channels,
                                                                     len, out_fmt, 1);
                        if (dst_bufsize > 0)
                        {
                            QByteArray atemp = QByteArray(reinterpret_cast<const char *>(audio_out_buffer), dst_bufsize);
                            byteBuf.append(atemp);
                        }
                    }
                }
            }
        }
    }
    else if (packet->stream_index == videoindex)
    {
        // 视频解码
        ret = avcodec_send_packet(pCodecCtx, packet);
        if (ret >= 0)
        {
            while (ret >= 0)
            {
                ret = avcodec_receive_frame(pCodecCtx, pFrame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0)
                {
                    qWarning() << "视频解码失败.";
                    break;
                }

                // 像素格式转换
                sws_scale(img_convert_ctx, (const unsigned char* const*)pFrame->data,
                          pFrame->linesize, 0, pCodecCtx->height,
                          pFrameRGB->data, pFrameRGB->linesize);

                QImage img((uchar*)pFrameRGB->data[0], pCodecCtx->width, pCodecCtx->height, QImage::Format_RGB32);
                img = img.scaled(videoW, videoH);
                QPixmap temp = QPixmap::fromImage(img);
                ui->videoLabel->setPixmap(temp);

                // 根据 PTS 更新当前时间
                if (pFrame->pts != AV_NOPTS_VALUE)
                {
                    qint64 secs = pFrame->pts * av_q2d(pFormatCtx->streams[videoindex]->time_base);
                    m_nCurrentMs = secs * 1000;
                    if (m_nCurrentMs < 0) m_nCurrentMs = 0;
                    if (m_nCurrentMs > m_nDurationMs) m_nCurrentMs = m_nDurationMs;
                }
            }
        }
    }

    av_packet_unref(packet);

    // 节奏控制
    Delay_MSec(frameRate - 5);
    return true;
}

int MainWindow::showVideo(char *szFileData, qint64 nFileLen)
{
    videoW = ui->videoLabel->size().width();
    videoH = ui->videoLabel->size().height();

    if (timer->isActive()) timer->stop();

    // 清理上次播放的资源
    clearFFmpegResources();
    av_channel_layout_default(&out_ch_layout, 2);

    // 音频输出设置
    QAudioFormat fmt;
    fmt.setSampleRate(44100);
    fmt.setSampleSize(16);
    fmt.setChannelCount(2);
    fmt.setCodec("audio/pcm");
    fmt.setByteOrder(QAudioFormat::LittleEndian);
    fmt.setSampleType(QAudioFormat::SignedInt);
    audioOutput = new QAudioOutput(fmt);
    audioOutput->setVolume(m_nVolume / 100.0f);
    streamOut = audioOutput->start();

    avformat_network_init();
    pFormatCtx = avformat_alloc_context();

    // 将传入的数据拷贝到 AVIOContext（FFmpeg 内部管理）
    unsigned char *aviobuffer = (unsigned char *)av_malloc(nFileLen);
    memcpy(aviobuffer, szFileData, nFileLen);
    AVIOContext *avio = avio_alloc_context(aviobuffer, nFileLen, 0, nullptr, nullptr, nullptr, nullptr);
    pFormatCtx->pb = avio;

    if (avformat_open_input(&pFormatCtx, nullptr, nullptr, nullptr) != 0)
    {
        qWarning() << "Couldn't open input stream.";
        return -1;
    }

    if (avformat_find_stream_info(pFormatCtx, nullptr) < 0)
    {
        qWarning() << "媒体流获取失败.";
        return -1;
    }

    videoindex = -1;
    audioindex = -1;

    // 查找视频流（使用 codecpar 而非废弃的 ->codec）
    for (int i = 0; i < (int)pFormatCtx->nb_streams; i++)
    {
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            videoindex = i;
            break;
        }
    }
    if (videoindex == -1)
    {
        qWarning() << "找不到视频流.";
        return -1;
    }

    // 查找音频流
    for (int i = 0; i < (int)pFormatCtx->nb_streams; i++)
    {
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            audioindex = i;
            break;
        }
    }
    if (audioindex == -1)
    {
        qWarning() << "找不到音频流.";
        return -1;
    }

    // ========== 视频解码 ==========
    pCodecCtx = avcodec_alloc_context3(nullptr);
    avcodec_parameters_to_context(pCodecCtx, pFormatCtx->streams[videoindex]->codecpar);

    // 获取帧率
    AVRational frame_rate = av_guess_frame_rate(pFormatCtx, pFormatCtx->streams[videoindex], nullptr);
    float fps = av_q2d(frame_rate);
    if (fps > 100) fps = fps / 1001;
    if (fps <= 0) fps = 25;
    frameRate = static_cast<int>(1000.0 / fps);
    qDebug() << "帧/秒 = " << fps << " 播放间隔 = " << frameRate << "ms";

    // 打开视频解码器
    const AVCodec *videoCodec = avcodec_find_decoder(pCodecCtx->codec_id);
    if (!videoCodec)
    {
        qWarning() << "找不到视频解码器.";
        return -1;
    }
    if (avcodec_open2(pCodecCtx, videoCodec, nullptr) < 0)
    {
        qWarning() << "打开视频解码器失败.";
        return -1;
    }

    // ========== 音频解码 ==========
    aCodecCtx = avcodec_alloc_context3(nullptr);
    avcodec_parameters_to_context(aCodecCtx, pFormatCtx->streams[audioindex]->codecpar);

    const AVCodec *audioCodec = avcodec_find_decoder(aCodecCtx->codec_id);
    if (!audioCodec)
    {
        qWarning() << "找不到音频解码器.";
        return -1;
    }
    if (avcodec_open2(aCodecCtx, audioCodec, nullptr) < 0)
    {
        qWarning() << "打开音频解码器失败.";
        return -1;
    }

    // 清空音频缓存
    byteBuf.clear();

    // 创建帧
    pFrame    = av_frame_alloc();
    pFrameRGB = av_frame_alloc();

    // 音频重采样设置
    const AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16;
    out_sample_rate = aCodecCtx->sample_rate;

    // 输出缓冲区
    audio_out_buffer = (uint8_t *)av_malloc(MAX_AUDIO_FRAME_SIZE * 2);

    // 重采样上下文
    swr_ctx = nullptr;
    int swr_ret = swr_alloc_set_opts2(&swr_ctx, &out_ch_layout, out_sample_fmt, out_sample_rate,
                                      &aCodecCtx->ch_layout, aCodecCtx->sample_fmt,
                                      aCodecCtx->sample_rate, 0, nullptr);
    if (swr_ret < 0 || !swr_ctx)
    {
        qWarning() << "音频重采样上下文创建失败.";
    }
    else
    {
        swr_init(swr_ctx);
    }

    // 创建视频输出缓冲区
    int out_buffer_size = av_image_get_buffer_size(AV_PIX_FMT_RGB32, pCodecCtx->width, pCodecCtx->height, 1);
    out_buffer = (unsigned char *)av_malloc(out_buffer_size);
    av_image_fill_arrays(pFrameRGB->data, pFrameRGB->linesize, out_buffer,
                         AV_PIX_FMT_RGB32, pCodecCtx->width, pCodecCtx->height, 1);

    // 创建 AVPacket
    packet = av_packet_alloc();

    // 像素格式转换上下文
    img_convert_ctx = sws_getContext(
        pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt,
        pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_RGB32,
        SWS_BICUBIC, nullptr, nullptr, nullptr);

    // 总时长(毫秒)
    m_nDurationMs = 0;

    // 1. 尝试 format-level duration (微秒)
    if (pFormatCtx->duration > 0)
        m_nDurationMs = pFormatCtx->duration / 1000;

    // 2. 尝试 video stream duration
    if (m_nDurationMs <= 0 && pFormatCtx->streams[videoindex]->duration > 0)
        m_nDurationMs = (qint64)(pFormatCtx->streams[videoindex]->duration *
                                  av_q2d(pFormatCtx->streams[videoindex]->time_base) * 1000);

    // 3. 保底: 用第一条视频流 PTS 时长，若仍为 0 则保守设为 1 分钟
    if (m_nDurationMs <= 0) m_nDurationMs = 60000;
    qDebug() << "总时长(ms) = " << m_nDurationMs;

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

    // 开始逐帧播放
    m_bPlaying = true;
    timer->start(frameRate);

    return 0;
}

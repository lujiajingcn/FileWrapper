#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QFile>
#include <QDebug>
#include <QCoreApplication>

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    timer = new QTimer(this);
    timer->setTimerType(Qt::PreciseTimer);
    connect(timer, SIGNAL(timeout()), this, SLOT(timeCallback()));
}

void MainWindow::timeCallback(void)
{
    // 音频缓存播放
    if (audioOutput && audioOutput->state() != QAudio::StoppedState
     && audioOutput->state() != QAudio::SuspendedState)
    {
        int writeBytes = qMin(byteBuf.length(), audioOutput->bytesFree());
        streamOut->write(byteBuf.data(), writeBytes);
        byteBuf = byteBuf.right(byteBuf.length() - writeBytes);
    }
}

void Delay_MSec(unsigned int msec)
{
    QTime _Timer = QTime::currentTime().addMSecs(msec);
    while (QTime::currentTime() < _Timer)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
}

void MainWindow::resizeEvent(QResizeEvent*)
{
    qDebug() << "resize";
}

void MainWindow::clearFFmpegResources()
{
    if (pFrameRGB)  { av_frame_free(&pFrameRGB);  pFrameRGB  = nullptr; }
    if (pFrame)     { av_frame_free(&pFrame);     pFrame     = nullptr; }
    if (pCodecCtx)  { avcodec_free_context(&pCodecCtx);  pCodecCtx = nullptr; }
    if (aCodecCtx)  { avcodec_free_context(&aCodecCtx);  aCodecCtx = nullptr; }
    if (pFormatCtx) { avformat_close_input(&pFormatCtx); pFormatCtx = nullptr; }
    videoindex = -1;
    audioindex = -1;
}

MainWindow::~MainWindow()
{
    if (timer->isActive()) timer->stop();
    clearFFmpegResources();
    delete ui;
}

int MainWindow::showVideo(char *szFileData, qint64 nFileLen)
{
    videoW = ui->widget->size().width();
    videoH = ui->widget->size().height();

    if (timer->isActive()) timer->stop();

    // 清理上次播放的资源
    clearFFmpegResources();

    // 音频输出设置
    QAudioFormat fmt;
    fmt.setSampleRate(44100);
    fmt.setSampleSize(16);
    fmt.setChannelCount(2);
    fmt.setCodec("audio/pcm");
    fmt.setByteOrder(QAudioFormat::LittleEndian);
    fmt.setSampleType(QAudioFormat::SignedInt);
    audioOutput = new QAudioOutput(fmt);
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
    // 从 codecpar 创建解码上下文（替代废弃的 ->codec）
    pCodecCtx = avcodec_alloc_context3(nullptr);
    avcodec_parameters_to_context(pCodecCtx, pFormatCtx->streams[videoindex]->codecpar);

    // 获取帧率（替代直接访问 pCodecCtx->framerate）
    AVRational frame_rate = av_guess_frame_rate(pFormatCtx, pFormatCtx->streams[videoindex], nullptr);
    float fps = av_q2d(frame_rate);
    if (fps > 100) fps = fps / 1001;
    if (fps <= 0) fps = 25;
    int frameRate = static_cast<int>(1000.0 / fps);
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

    // 音频重采样设置（使用 AVChannelLayout 替代 uint64_t）
    AVChannelLayout out_ch_layout;
    av_channel_layout_default(&out_ch_layout, 2); // 立体声
    AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16;
    int out_sample_rate = aCodecCtx->sample_rate;

    // 输出缓冲区
    uint8_t *audio_out_buffer = (uint8_t *)av_malloc(MAX_AUDIO_FRAME_SIZE * 2);

    // 重采样上下文（使用 swr_alloc_set_opts2 分配并设置参数）
    SwrContext *swr_ctx = nullptr;
    int swr_ret = swr_alloc_set_opts2(&swr_ctx, &out_ch_layout, out_sample_fmt, out_sample_rate,
                                      &aCodecCtx->ch_layout, aCodecCtx->sample_fmt,
                                      aCodecCtx->sample_rate, 0, nullptr);
    if (swr_ret < 0 || !swr_ctx)
    {
        qWarning() << "音频重采样上下文创建失败.";
        // 不中断播放，仅跳过音频
    }
    else
    {
        swr_init(swr_ctx);
    }

    // 创建视频输出缓冲区
    int out_buffer_size = av_image_get_buffer_size(AV_PIX_FMT_RGB32, pCodecCtx->width, pCodecCtx->height, 1);
    unsigned char *out_buffer = (unsigned char *)av_malloc(out_buffer_size);
    av_image_fill_arrays(pFrameRGB->data, pFrameRGB->linesize, out_buffer,
                         AV_PIX_FMT_RGB32, pCodecCtx->width, pCodecCtx->height, 1);

    // 创建 AVPacket（使用 av_packet_alloc 而非 av_malloc(sizeof)）
    AVPacket *packet = av_packet_alloc();

    // 像素格式转换上下文
    struct SwsContext *img_convert_ctx = sws_getContext(
        pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt,
        pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_RGB32,
        SWS_BICUBIC, nullptr, nullptr, nullptr);

    timer->start(frameRate);

    // ========== 解码循环 ==========
    while (av_read_frame(pFormatCtx, packet) >= 0)
    {
        if (packet->stream_index == audioindex)
        {
            // 音频解码：send/receive API（替代 avcodec_decode_audio4）
            int ret = avcodec_send_packet(aCodecCtx, packet);
            if (ret < 0) { av_packet_unref(packet); continue; }

            while (ret >= 0)
            {
                ret = avcodec_receive_frame(aCodecCtx, pFrame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0)
                {
                    qWarning() << "音频解码失败.";
                    break;
                }

                // 音频重采样
                if (swr_ctx)
                {
                    int len = swr_convert(swr_ctx, &audio_out_buffer, MAX_AUDIO_FRAME_SIZE,
                                          (const uint8_t **)pFrame->data, pFrame->nb_samples);
                    if (len > 0)
                    {
                        int dst_bufsize = av_samples_get_buffer_size(nullptr, out_ch_layout.nb_channels,
                                                                     len, out_sample_fmt, 1);
                        QByteArray atemp = QByteArray(reinterpret_cast<const char *>(audio_out_buffer), dst_bufsize);
                        byteBuf.append(atemp);
                    }
                }
            }
        }
        else if (packet->stream_index == videoindex)
        {
            // 视频解码：send/receive API（替代 avcodec_decode_video2）
            int ret = avcodec_send_packet(pCodecCtx, packet);
            if (ret < 0) { av_packet_unref(packet); continue; }

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
                ui->label->setPixmap(temp);
                Delay_MSec(frameRate - 5);
            }
        }

        // 释放 Packet（替代 av_free_packet）
        av_packet_unref(packet);
    }

    // ========== 清理 FFmpeg 资源 ==========
    if (swr_ctx) swr_free(&swr_ctx);
    av_freep(&audio_out_buffer);
    sws_freeContext(img_convert_ctx);
    av_freep(&out_buffer);
    av_packet_free(&packet);
    av_channel_layout_uninit(&out_ch_layout);
    clearFFmpegResources();

    return 0;
}

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QFile>
#include <QDebug>


MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    timer = new QTimer(this);
    timer->setTimerType(Qt::PreciseTimer);   // 精准定时设置
    connect(timer,SIGNAL(timeout()),this,SLOT(timeCallback()));

}

void MainWindow::timeCallback(void)
{
    // 音频缓存播放
    if(audioOutput && audioOutput->state() != QAudio::StoppedState && audioOutput->state() != QAudio::SuspendedState)
    {
        int writeBytes = qMin(byteBuf.length(), audioOutput->bytesFree());
        streamOut->write(byteBuf.data(), writeBytes);
        byteBuf = byteBuf.right(byteBuf.length() - writeBytes);
    }
}

void Delay_MSec(unsigned int msec)
{
    QTime _Timer = QTime::currentTime().addMSecs(msec);
    while( QTime::currentTime() < _Timer )
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
}

void MainWindow::resizeEvent(QResizeEvent* )
{
    qDebug()<<"resize";
}

MainWindow::~MainWindow()
{
    delete ui;
}

int MainWindow::showVideo(char *szFileData, int nFileLen)
{
    videoW = ui->widget->size().width();
    videoH = ui->widget->size().height();

    if(timer->isActive())   timer->stop();

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

    //Init AVIOContext
    unsigned char *aviobuffer = (unsigned char *)av_malloc(nFileLen);
    memcpy(aviobuffer, szFileData, nFileLen);
    AVIOContext *avio = avio_alloc_context(aviobuffer, nFileLen, 0, nullptr, nullptr, nullptr, nullptr);
    pFormatCtx->pb = avio;

    if(avformat_open_input(&pFormatCtx, nullptr, nullptr, nullptr) != 0){
        printf("Couldn't open input stream.\n");
        return -1;
    }

    // 获取音视频流
    if(avformat_find_stream_info(pFormatCtx, nullptr) < 0){
        qDebug("媒体流获取失败.\n");
        return -1;
    }
    videoindex = -1;
    audioindex = -1;
    //nb_streams视音频流的个数，这里当查找到视频流时就中断了。
    for(int i=0; i < pFormatCtx->nb_streams; i++)
        if(pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO){
            videoindex=i;
            break;
    }
    if(videoindex==-1){
        qDebug("找不到视频流.\n");
        return -1;
    }

    //nb_streams视音频流的个数，这里当查找到音频流时就中断了。
    for(int i=0; i < pFormatCtx->nb_streams; i++)
        if(pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO){
            audioindex=i;
            break;
    }
    if(audioindex == -1){
        qDebug("找不到音频流.\n");
        return -1;
    }

    //获取视频流编码结构
    pCodecCtx=pFormatCtx->streams[videoindex]->codec;

    float frameNum = pCodecCtx->framerate.num;  // 每秒帧数
    if(frameNum > 100)  frameNum = frameNum/1001;
    int frameRate = 1000/frameNum;   //
//    int frameRate = 40;
    qDebug("帧/秒 = %f  播放间隔是时间=%d\n",frameNum,frameRate);

    //查找解码器
    pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
    if(pCodec == nullptr)
    {
        qDebug("找不到解码器.\n");
        return -1;
    }
    //使用解码器读取pCodecCtx结构
    if(avcodec_open2(pCodecCtx, pCodec, nullptr) < 0)
    {
        qDebug("打开视频码流失败.\n");
        return -1;
    }

    //获取音频流编码结构-------------------------------------------------------------
    aCodecParameters = pFormatCtx->streams[audioindex]->codecpar;
    aCodec = avcodec_find_decoder(aCodecParameters->codec_id);
    if (aCodec == nullptr) {
        qDebug("找不到解码器.\n");
        return -1;
    }
    aCodecCtx = avcodec_alloc_context3(aCodec);
    avcodec_parameters_to_context(aCodecCtx, aCodecParameters);
    //使用解码器读取aCodecCtx结构
    if (avcodec_open2(aCodecCtx, aCodec, nullptr) < 0) {
        qDebug("打开视频码流失败.\n");
        return 0;
    }

    // 清空缓存区
    byteBuf.clear();

    //创建帧结构，此函数仅分配基本结构空间，图像数据空间需通过av_malloc分配
    pFrame = av_frame_alloc();
    pFrameRGB = av_frame_alloc();

    // 获取音频参数
    uint64_t out_channel_layout = aCodecCtx->channel_layout;
    AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16;
    int out_sample_rate = aCodecCtx->sample_rate;
    int out_channels = av_get_channel_layout_nb_channels(out_channel_layout);

    uint8_t *audio_out_buffer = (uint8_t *)av_malloc(MAX_AUDIO_FRAME_SIZE*2);
    SwrContext *swr_ctx = swr_alloc_set_opts(nullptr, out_channel_layout, out_sample_fmt, out_sample_rate, aCodecCtx->channel_layout, aCodecCtx->sample_fmt, aCodecCtx->sample_rate, 0, nullptr);
    swr_init(swr_ctx);

    //创建动态内存,创建存储图像数据的空间
    unsigned char *out_buffer = (unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_RGB32, pCodecCtx->width, pCodecCtx->height, 1));
    av_image_fill_arrays(pFrameRGB->data, pFrameRGB->linesize, out_buffer, AV_PIX_FMT_RGB32, pCodecCtx->width, pCodecCtx->height, 1);
    AVPacket *packet = (AVPacket *)av_malloc(sizeof(AVPacket));
    //初始化img_convert_ctx结构
    struct SwsContext *img_convert_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_RGB32, SWS_BICUBIC, nullptr, nullptr, nullptr);

    timer->start(frameRate);  //定时间隔播放

    while (av_read_frame(pFormatCtx, packet) >= 0){
        if (packet->stream_index == audioindex){
            int ret = avcodec_decode_audio4(aCodecCtx, pFrame, &got_audio, packet);
            if ( ret < 0)
            {
                qDebug("解码失败.\n");
                return 0;
            }

            if (got_audio)
            {
                int len = swr_convert(swr_ctx, &audio_out_buffer, MAX_AUDIO_FRAME_SIZE, (const uint8_t **)pFrame->data, pFrame->nb_samples);
                if (len <= 0)
                {
                    continue;
                }
                int dst_bufsize = av_samples_get_buffer_size(0, out_channels, len, out_sample_fmt, 1);
                QByteArray atemp =  QByteArray((const char *)audio_out_buffer, dst_bufsize);
                byteBuf.append(atemp);
            }
        }
        //如果是视频数据
        else if (packet->stream_index == videoindex){
            //解码一帧视频数据
            ret = avcodec_decode_video2(pCodecCtx, pFrame, &got_picture, packet);

            if (ret < 0){
                qDebug("解码失败.\n");
                return 0;
            }
            if (got_picture){
                sws_scale(img_convert_ctx, (const unsigned char* const*)pFrame->data, pFrame->linesize, 0, pCodecCtx->height,
                    pFrameRGB->data, pFrameRGB->linesize);
                QImage img((uchar*)pFrameRGB->data[0],pCodecCtx->width,pCodecCtx->height,QImage::Format_RGB32);
                img = img.scaled(videoW, videoH);
                QPixmap temp = QPixmap::fromImage(img);
                ui->label->setPixmap(temp);
                Delay_MSec(frameRate-5);  // 这里需要流出时间来显示，如果不要这个延时界面回卡死到整个视频解码完成才能播放显示
            }
        }
        av_free_packet(packet);
    }

    sws_freeContext(img_convert_ctx);
    av_frame_free(&pFrameRGB);
    av_frame_free(&pFrame);
    avcodec_close(pCodecCtx);
    avformat_close_input(&pFormatCtx);
}

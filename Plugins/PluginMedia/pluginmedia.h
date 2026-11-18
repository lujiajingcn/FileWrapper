#ifndef PLUGINMEDIA_H
#define PLUGINMEDIA_H

#include "mainwindow.h"
#include "pluginbase.h"

// 代理层样板见 Plugins/common/pluginbase.h；本类额外实现：
//   * 后台播放相关（stopPlayback / hasBackgroundPlayback）
//   * 大文件走路径（prefersFileByPath / sendFileByPath）
//   * showVideo 信号（解码在窗口侧完成，构造时连接）
class PluginMedia : public PluginBase<PluginMedia, MainWindow>
{
    Q_OBJECT
    Q_INTERFACES(PluginInterface)
    Q_PLUGIN_METADATA(IID "fileWrapper.pluginMedia" FILE "pluginMedia.json")
    using Base = PluginBase<PluginMedia, MainWindow>;
public:
    explicit PluginMedia(QObject *parent = nullptr);

    void stopPlayback() override;
    bool stopPlaybackAndWait() override;
    bool hasBackgroundPlayback() const override;

    // 大文件走文件路径通道，FFmpeg 直接流式读取，避免整块载入内存（P1-2）
    bool prefersFileByPath() const override;
    bool sendFileByPath(const QString &path, const QString &displayName) override;

    // v4：主程序把归档内容区 mmap 后交进来，插件直接引用（播放期间宿主保持映射）
    bool sendFileMapped(char *pData, qint64 nFileLen, const QString &displayName) override;

signals:
    void showVideo(char *szFileData, qint64 nFileLen);

protected:
    bool showFile(const PluginFile &file) override;
};

#endif // PLUGINMEDIA_H

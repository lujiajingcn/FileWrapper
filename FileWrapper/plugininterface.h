#ifndef PLUGININTERFACE_H
#define PLUGININTERFACE_H

#include <QObject>
#include <QWidget>

class PluginInterface
{
public:
    virtual ~PluginInterface(){}
    virtual void sendFileData(char *szFileData, qint64 nFileLen) = 0;
    virtual QWidget* getPluginWidget() = 0;

    // 切换文件时由主程序调用：停止该插件正在进行的后台播放（视频/音频等）。
    // 默认空实现，只有连续播放类插件（如 PluginMedia）需要覆盖。
    virtual void stopPlayback() {}
};

#define PluginTemplate_main_iid "FileWrapper.main"

QT_BEGIN_NAMESPACE
Q_DECLARE_INTERFACE(PluginInterface, PluginTemplate_main_iid)
QT_END_NAMESPACE

#endif // PLUGININTERFACE_H

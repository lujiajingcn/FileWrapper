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

    // 该插件是否存在“后台播放”（音视频解码线程 / 音频输出等持续占用的运行态）。
    // 主程序在切换标签页时会停止上一实例的播放；对这类插件，被停止的标签页
    // 在再次激活时需要重新渲染（解码资源已释放），故据此让该页的渲染缓存失效。
    // 默认 false，只有流媒体类插件（如 PluginMedia）需要覆盖为 true。
    virtual bool hasBackgroundPlayback() const { return false; }

    // 由主程序在“新建标签页 / 在该标签页打开文件”时调用：为该标签页创建一个**独立**的
    // 插件实例，使同一插件类型的多个标签页各自拥有独立的内容与窗口，互不干扰。
    // 返回 nullptr 表示该插件不支持多实例，主程序将回退到共享实例（此时同类型多标签页共用内容）。
    // 所有权：调用方负责在用完后 delete 返回的实例（PluginInterface 具有虚析构函数）。
    virtual PluginInterface* createInstance() { return nullptr; }
};

#define PluginTemplate_main_iid "FileWrapper.main"

QT_BEGIN_NAMESPACE
Q_DECLARE_INTERFACE(PluginInterface, PluginTemplate_main_iid)
QT_END_NAMESPACE

#endif // PLUGININTERFACE_H

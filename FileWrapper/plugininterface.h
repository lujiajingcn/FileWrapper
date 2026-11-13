#ifndef PLUGININTERFACE_H
#define PLUGININTERFACE_H

#include <QObject>
#include <QWidget>
#include <QString>

class PluginInterface
{
public:
    virtual ~PluginInterface(){}
    virtual void sendFileData(char *szFileData, qint64 nFileLen) = 0;
    virtual QWidget* getPluginWidget() = 0;

    // 切换文件时由主程序调用：停止该插件正在进行的后台播放（视频/音频等）。
    virtual void stopPlayback() {}

    // 该插件是否存在「后台播放」（音视频解码线程 / 音频输出等持续占用的运行态）。
    virtual bool hasBackgroundPlayback() const { return false; }

    // 由主程序在「新建标签页 / 在该标签页打开文件」时调用：创建独立的插件实例。
    virtual PluginInterface* createInstance() { return nullptr; }

    // —— 以下为可选扩展，默认实现保持旧插件行为完全不变 ——

    // 大文件（>= 阈值）时主程序把归档内容流式抽取到临时文件，优先走此接口，
    // 避免把数 GB 数据一次性读进内存再复制一份（P1-2）。
    // 返回 true 表示插件已自行处理该文件；返回 false 时主程序回退到内存路径。
    // 注意：path 指向的文件在本次会话结束前一直有效（Plugins 可在后台线程持续读取）。
    virtual bool prefersFileByPath() const { return false; }
    virtual bool sendFileByPath(const QString &path, const QString &displayName)
    {
        Q_UNUSED(path); Q_UNUSED(displayName);
        return false;
    }

    // 主程序在送数据前告知文件名，插件可据此做标题回退（P2-7）
    virtual void setSourceName(const QString &name) { Q_UNUSED(name); }
};

#define PluginTemplate_main_iid "FileWrapper.main"

QT_BEGIN_NAMESPACE
Q_DECLARE_INTERFACE(PluginInterface, PluginTemplate_main_iid)
QT_END_NAMESPACE

#endif // PLUGININTERFACE_H

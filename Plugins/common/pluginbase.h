#ifndef PLUGINBASE_H
#define PLUGINBASE_H

#include "plugininterface.h"

// 插件代理层基类（CRTP）—— 收口 6 个插件里重复的样板。
//
// 每个插件的代理类（pluginxxx.h/.cpp）原先都要重复这些内容：
//   构造时 new 内部窗口、析构 delete、apiVersion()、getPluginWidget()、
//   createInstance()、sendFileData() 的空值处理。
// 现在只保留插件真正不同的部分：元数据（Q_PLUGIN_METADATA）与"怎么把文件显示出来"。
//
// 用法：
//   class PluginText : public PluginBase<PluginText, MainWindow>
//   {
//       Q_OBJECT
//       Q_INTERFACES(PluginInterface)
//       Q_PLUGIN_METADATA(IID "fileWrapper.pluginText" FILE "pluginText.json")
//       using Base = PluginBase<PluginText, MainWindow>;
//   public:
//       using Base::Base;                 // 继承带 QObject parent 的构造
//   protected:
//       bool showFile(const PluginFile &file) override;   // 唯一必须实现的钩子
//   };
//
// 需要额外能力时照旧重写接口虚函数：
//   * 有后台播放：stopPlayback() / hasBackgroundPlayback()
//   * 大文件走路径：prefersFileByPath() / sendFileByPath()
//   * 要显示文件名：setSourceName()
//   * 特殊构造（如 connect 信号）：自己写构造函数，初始化列表里调 Base(parent)
//
// 为什么是模板而不是普通基类：createInstance() 必须返回**具体插件类型**；
// 模板让基类能在编译期生成 `new Derived`，不需要每个插件再抄一遍。
template <typename Derived, typename WindowT>
class PluginBase : public QObject, public PluginInterface
{
public:
    explicit PluginBase(QObject *parent = nullptr)
        : QObject(parent), m_wMainWindow(new WindowT)
    {
    }

    ~PluginBase() override
    {
        delete m_wMainWindow;
        m_wMainWindow = nullptr;
    }

    // 接口版本：与主程序 include/plugininterface.h 的 PLUGIN_API_VERSION 同源（B1）
    int apiVersion() const override { return PLUGIN_API_VERSION; }

    QWidget *getPluginWidget() override { return m_wMainWindow; }

    // 每个标签页都要独立实例（B2：返回 nullptr 会被主程序判为打开失败）
    PluginInterface *createInstance() override { return new Derived; }

    // 统一数据通道（C2）：路径型插件先走自己的路径接口，其余交给 showFile()；
    // 空数据视为"清屏"。
    bool sendFile(const PluginFile &file) override
    {
        if (file.isPath() && prefersFileByPath())
            return sendFileByPath(file.path(), file.displayName());

        if (file.isEmpty())
        {
            m_wMainWindow->clearContent();
            return false;
        }
        return showFile(file);
    }

    // 旧通道（v1）：主程序/旧调用方仍可能用它，这里适配到 showFile()。
    void sendFileData(char *szFileData, qint64 nFileLen) override
    {
        if (szFileData == nullptr || nFileLen <= 0)
        {
            m_wMainWindow->clearContent();
            return;
        }
        showFile(PluginFile::fromMemory(QByteArray(szFileData, qsizetype(nFileLen))));
    }

protected:
    // 各插件实现：把 file 的内容显示到内部窗口里。
    // 返回 true 表示已处理；false 表示放弃（主程序可能回退/报错）。
    virtual bool showFile(const PluginFile &file) = 0;

    WindowT *m_wMainWindow = nullptr;
};

#endif // PLUGINBASE_H

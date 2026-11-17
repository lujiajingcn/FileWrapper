#ifndef PLUGINTEXT_H
#define PLUGINTEXT_H

#include "mainwindow.h"
#include "pluginbase.h"

// 代理层样板（构造/析构、apiVersion、getPluginWidget、createInstance、旧通道适配）
// 都在 Plugins/common/pluginbase.h 里；本类只需实现"怎么显示文件"。
class PluginText : public PluginBase<PluginText, MainWindow>
{
    Q_OBJECT
    Q_INTERFACES(PluginInterface)
    Q_PLUGIN_METADATA(IID "fileWrapper.pluginText" FILE "pluginText.json")
    using Base = PluginBase<PluginText, MainWindow>;
public:
    using Base::Base;

protected:
    bool showFile(const PluginFile &file) override;
};

#endif // PLUGINTEXT_H

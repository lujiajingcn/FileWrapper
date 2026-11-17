#ifndef PLUGINPICTURE_H
#define PLUGINPICTURE_H

#include "mainwindow.h"
#include "pluginbase.h"

// 代理层样板见 Plugins/common/pluginbase.h；本类只实现 showFile()。
class PluginPicture : public PluginBase<PluginPicture, MainWindow>
{
    Q_OBJECT
    Q_INTERFACES(PluginInterface)
    Q_PLUGIN_METADATA(IID "fileWrapper.pluginPicture" FILE "pluginPicture.json")
    using Base = PluginBase<PluginPicture, MainWindow>;
public:
    using Base::Base;

protected:
    bool showFile(const PluginFile &file) override;
};

#endif // PLUGINPICTURE_H

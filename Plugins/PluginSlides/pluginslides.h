#ifndef PLUGINSLIDES_H
#define PLUGINSLIDES_H

#include "mainwindow.h"
#include "pluginbase.h"

// 演示查看插件：.pptx（OOXML PresentationML）与 .odp（OpenDocument Presentation）。
// 代理层样板见 Plugins/common/pluginbase.h；本类只实现 showFile()。
class PluginSlides : public PluginBase<PluginSlides, MainWindow>
{
    Q_OBJECT
    Q_INTERFACES(PluginInterface)
    Q_PLUGIN_METADATA(IID "fileWrapper.pluginSlides" FILE "pluginSlides.json")
    using Base = PluginBase<PluginSlides, MainWindow>;
public:
    using Base::Base;

    void setSourceName(const QString &name) override;

protected:
    bool showFile(const PluginFile &file) override;
};

#endif // PLUGINSLIDES_H

#ifndef PLUGINBOOK_H
#define PLUGINBOOK_H

#include "mainwindow.h"
#include "pluginbase.h"

// 代理层样板见 Plugins/common/pluginbase.h；本类额外提供 setSourceName（书名回退）。
class PluginBook : public PluginBase<PluginBook, MainWindow>
{
    Q_OBJECT
    Q_INTERFACES(PluginInterface)
    Q_PLUGIN_METADATA(IID "fileWrapper.pluginBook" FILE "pluginBook.json")
    using Base = PluginBase<PluginBook, MainWindow>;
public:
    using Base::Base;

    void setSourceName(const QString &name) override;

protected:
    bool showFile(const PluginFile &file) override;
};

#endif // PLUGINBOOK_H

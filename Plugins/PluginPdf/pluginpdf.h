#ifndef PLUGINPDF_H
#define PLUGINPDF_H

#include "mainwindow.h"
#include "pluginbase.h"

// 代理层样板见 Plugins/common/pluginbase.h；本类额外实现"大文件走路径"通道。
class PluginPdf : public PluginBase<PluginPdf, MainWindow>
{
    Q_OBJECT
    Q_INTERFACES(PluginInterface)
    Q_PLUGIN_METADATA(IID "fileWrapper.pluginPdf" FILE "pluginPdf.json")
    using Base = PluginBase<PluginPdf, MainWindow>;
public:
    using Base::Base;

    // 大文件（>= 阈值）由主程序抽到临时文件后走这里，QPdfDocument 直接按路径打开
    bool prefersFileByPath() const override;
    bool sendFileByPath(const QString &path, const QString &displayName) override;

    // 打开失败的真实原因（损坏/加密/读不到）；宿主据此显示可照做的提示
    QString lastError() const override;

protected:
    bool showFile(const PluginFile &file) override;
};

#endif // PLUGINPDF_H

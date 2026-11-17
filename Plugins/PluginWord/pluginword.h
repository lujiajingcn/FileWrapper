#ifndef PLUGINWORD_H
#define PLUGINWORD_H

#include "mainwindow.h"
#include "pluginbase.h"

// Word 文档插件：主程序按后缀（docx/docm/dotx/dotm，以及给旧版 doc 提示）
// 选中本插件，插件把整份文档字节交给 MainWindow 解析显示。
//
// 代理层样板见 Plugins/common/pluginbase.h；本类的 showFile() 直接吃
// PluginFile 的内存块（C2 零拷贝），并额外提供 setSourceName（标题回退）。
class PluginWord : public PluginBase<PluginWord, MainWindow>
{
    Q_OBJECT
    Q_INTERFACES(PluginInterface)
    Q_PLUGIN_METADATA(IID "fileWrapper.pluginWord" FILE "pluginWord.json")
    using Base = PluginBase<PluginWord, MainWindow>;
public:
    using Base::Base;

    void setSourceName(const QString &name) override;

protected:
    bool showFile(const PluginFile &file) override;
};

#endif // PLUGINWORD_H

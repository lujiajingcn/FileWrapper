#ifndef PLUGINSHEET_H
#define PLUGINSHEET_H

#include "mainwindow.h"
#include "pluginbase.h"

// 表格查看插件：.xlsx/.xlsm（OOXML SpreadsheetML）与 .ods（OpenDocument Spreadsheet）。
// 代理层样板见 Plugins/common/pluginbase.h；本类只实现 showFile()。
class PluginSheet : public PluginBase<PluginSheet, MainWindow>
{
    Q_OBJECT
    Q_INTERFACES(PluginInterface)
    Q_PLUGIN_METADATA(IID "fileWrapper.pluginSheet" FILE "pluginSheet.json")
    using Base = PluginBase<PluginSheet, MainWindow>;
public:
    using Base::Base;

    void setSourceName(const QString &name) override;

protected:
    bool showFile(const PluginFile &file) override;
};

#endif // PLUGINSHEET_H

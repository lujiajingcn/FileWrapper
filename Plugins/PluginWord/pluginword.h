#ifndef PLUGINWORD_H
#define PLUGINWORD_H

#include "mainwindow.h"

#include "plugininterface.h"

// Word 文档插件：主程序按后缀（docx/docm/dotx/dotm，以及给旧版 doc 提示）
// 选中本插件，插件把整份文档字节交给 MainWindow 解析显示。
class PluginWord : public QObject, public PluginInterface
{
    Q_OBJECT
    Q_INTERFACES(PluginInterface)
    Q_PLUGIN_METADATA(IID "fileWrapper.pluginWord" FILE "pluginWord.json")
public:
    explicit PluginWord(QObject *parent = nullptr);
    ~PluginWord();
    // 插件 API 版本（B1）：与主程序 plugininterface.h 的 PLUGIN_API_VERSION 一致
    int apiVersion() const override { return PLUGIN_API_VERSION; }
    void sendFileData(char *szFileData, qint64 nFileLen);
    QWidget* getPluginWidget();
    PluginInterface* createInstance();
    void setSourceName(const QString &name) override;
    // C2：统一数据通道，直接吃 PluginFile，不再经过 char* 复制一次
    bool sendFile(const PluginFile &file) override;

private:
    MainWindow* m_wMainWindow;
};

#endif // PLUGINWORD_H

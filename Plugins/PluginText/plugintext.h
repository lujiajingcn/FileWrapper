#ifndef PLUGINTEXT_H
#define PLUGINTEXT_H

#include "mainwindow.h"

#include "plugininterface.h"

class PluginText : public QObject,public PluginInterface
{
    Q_OBJECT
    Q_INTERFACES(PluginInterface)
    Q_PLUGIN_METADATA(IID "fileWrapper.pluginText" FILE "pluginText.json")
public:
    explicit PluginText(QObject *parent = nullptr);
    ~PluginText();
    // 插件 API 版本（B1）：与主程序 plugininterface.h 的 PLUGIN_API_VERSION 一致
    int apiVersion() const override { return PLUGIN_API_VERSION; }
    void sendFileData(char *szFileData, qint64 nFileLen);
    QWidget* getPluginWidget();
    PluginInterface* createInstance();

private:
    MainWindow*         m_wMainWindow;
};

#endif // PLUGINTEXT_H

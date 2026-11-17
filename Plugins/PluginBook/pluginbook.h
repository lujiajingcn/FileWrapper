#ifndef PLUGINBOOK_H
#define PLUGINBOOK_H

#include "mainwindow.h"

#include "plugininterface.h"

class PluginBook : public QObject, public PluginInterface
{
    Q_OBJECT
    Q_INTERFACES(PluginInterface)
    Q_PLUGIN_METADATA(IID "fileWrapper.pluginBook" FILE "pluginBook.json")
public:
    explicit PluginBook(QObject *parent = nullptr);
    ~PluginBook();
    // 插件 API 版本（B1）：与主程序 plugininterface.h 的 PLUGIN_API_VERSION 一致
    int apiVersion() const override { return PLUGIN_API_VERSION; }
    void sendFileData(char *szFileData, qint64 nFileLen);
    QWidget* getPluginWidget();
    PluginInterface* createInstance();
    void setSourceName(const QString &name) override;

private:
    MainWindow* m_wMainWindow;
};

#endif // PLUGINBOOK_H

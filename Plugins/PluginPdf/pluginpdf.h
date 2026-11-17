#ifndef PLUGINPDF_H
#define PLUGINPDF_H

#include "mainwindow.h"

#include "plugininterface.h"

class PluginPdf : public QObject,public PluginInterface
{
    Q_OBJECT
    Q_INTERFACES(PluginInterface)
    Q_PLUGIN_METADATA(IID "fileWrapper.pluginPdf" FILE "pluginPdf.json")
public:
    explicit PluginPdf(QObject *parent = nullptr);
    ~PluginPdf();
    // 插件 API 版本（B1）：与主程序 plugininterface.h 的 PLUGIN_API_VERSION 一致
    int apiVersion() const override { return PLUGIN_API_VERSION; }
    void sendFileData(char *szFileData, qint64 nFileLen);
    QWidget* getPluginWidget();
    PluginInterface* createInstance();
    bool prefersFileByPath() const override;
    bool sendFileByPath(const QString &path, const QString &displayName) override;

private:
    MainWindow*         m_wMainWindow;
};

#endif // PLUGINPDF_H

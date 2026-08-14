#ifndef PLUGINMEDIA_H
#define PLUGINMEDIA_H

#include "mainwindow.h"

#include "../../FileWrapper/plugininterface.h"

class PluginMedia : public QObject,public PluginInterface
{
    Q_OBJECT
    Q_INTERFACES(PluginInterface)
    Q_PLUGIN_METADATA(IID "fileWrapper.pluginMedia" FILE "pluginMedia.json")
public:
    explicit PluginMedia(QObject *parent = nullptr);
    ~PluginMedia();
    void sendFileData(char *szFileData, qint64 nFileLen);
    QWidget* getPluginWidget();
signals:
    void showVideo(char *szFileData, qint64 nFileLen);
private:
    MainWindow*         m_wMainWindow;
};

#endif // PLUGINMEDIA_H

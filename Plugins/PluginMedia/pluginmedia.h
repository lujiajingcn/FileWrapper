#ifndef PLUGINA_H
#define PLUGINA_H

#include "mainwindow.h"

#include "../../FileWrapper/plugininterface.h"

class PluginMedia : public QObject,public PluginInterface
{
    Q_OBJECT
    Q_INTERFACES(PluginInterface)
    Q_PLUGIN_METADATA(IID "fileWrapper.pluginMedia" FILE "pluginMedia.json")
public:
    explicit PluginMedia(QObject *parent = nullptr);
    void sendFileData(char *szFileData, int nFileLen);
    QWidget* getPluginWidget();
signals:
    void showVideo(char *szFileData, int nFileLen);
private:
    MainWindow*         m_wMainWindow;
};

#endif // PLUGINA_H

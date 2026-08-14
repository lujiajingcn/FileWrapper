#ifndef PLUGINPICTURE_H
#define PLUGINPICTURE_H

#include "mainwindow.h"

#include "../../FileWrapper/plugininterface.h"

class PluginPicture : public QObject,public PluginInterface
{
    Q_OBJECT
    Q_INTERFACES(PluginInterface)
    Q_PLUGIN_METADATA(IID "fileWrapper.pluginPicture" FILE "pluginPicture.json")
public:
    explicit PluginPicture(QObject *parent = nullptr);
    ~PluginPicture();
    void sendFileData(char *szFileData, qint64 nFileLen);
    QWidget* getPluginWidget();

private:
    MainWindow*         m_wMainWindow;
};

#endif // PLUGINPICTURE_H

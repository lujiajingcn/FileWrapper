#ifndef PLUGINTEXT_H
#define PLUGINTEXT_H

#include "mainwindow.h"

#include "../../FileWrapper/plugininterface.h"

class PluginText : public QObject,public PluginInterface
{
    Q_OBJECT
    Q_INTERFACES(PluginInterface)
    Q_PLUGIN_METADATA(IID "fileWrapper.pluginText" FILE "pluginText.json")
public:
    explicit PluginText(QObject *parent = nullptr);
    ~PluginText();
    void sendFileData(char *szFileData, qint64 nFileLen);
    QWidget* getPluginWidget();

private:
    MainWindow*         m_wMainWindow;
};

#endif // PLUGINTEXT_H

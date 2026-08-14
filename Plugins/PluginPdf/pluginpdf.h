#ifndef PLUGINPDF_H
#define PLUGINPDF_H

#include "mainwindow.h"

#include "../../FileWrapper/plugininterface.h"

class PluginPdf : public QObject,public PluginInterface
{
    Q_OBJECT
    Q_INTERFACES(PluginInterface)
    Q_PLUGIN_METADATA(IID "fileWrapper.pluginPdf" FILE "pluginPdf.json")
public:
    explicit PluginPdf(QObject *parent = nullptr);
    ~PluginPdf();
    void sendFileData(char *szFileData, qint64 nFileLen);
    QWidget* getPluginWidget();

private:
    MainWindow*         m_wMainWindow;
};

#endif // PLUGINPDF_H

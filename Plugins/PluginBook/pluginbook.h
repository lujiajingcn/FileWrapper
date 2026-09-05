#ifndef PLUGINBOOK_H
#define PLUGINBOOK_H

#include "mainwindow.h"

#include "../../FileWrapper/plugininterface.h"

class PluginBook : public QObject, public PluginInterface
{
    Q_OBJECT
    Q_INTERFACES(PluginInterface)
    Q_PLUGIN_METADATA(IID "fileWrapper.pluginBook" FILE "pluginBook.json")
public:
    explicit PluginBook(QObject *parent = nullptr);
    ~PluginBook();
    void sendFileData(char *szFileData, qint64 nFileLen);
    QWidget* getPluginWidget();

private:
    MainWindow* m_wMainWindow;
};

#endif // PLUGINBOOK_H

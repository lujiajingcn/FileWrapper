#ifndef PLUGININTERFACE_H
#define PLUGININTERFACE_H

#include <QObject>

class PluginInterface
{
public:
    virtual ~PluginInterface(){}
    virtual void sendFileData(char *szFileData, int nFileLen) = 0;
    virtual QWidget* getPluginWidget() = 0;
};

#define PluginTemplate_main_iid "FileWrapper.main"

QT_BEGIN_NAMESPACE
Q_DECLARE_INTERFACE(PluginInterface, PluginTemplate_main_iid)
QT_END_NAMESPACE

#endif // PLUGININTERFACE_H

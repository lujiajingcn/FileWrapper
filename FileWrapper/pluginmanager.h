#ifndef PLUGINMANAGER_H
#define PLUGINMANAGER_H

#include <QObject>
#include <QPluginLoader>
#include "plugininterface.h"
#include <QMap>

class PluginManager : public QObject
{
    Q_OBJECT

public:
    explicit PluginManager(QObject *parent = nullptr);

    static PluginManager *getInstance();

    bool loadPlugin();

    void loadAllPlugins();

    void scanMetaData(const QString &sPath);

    PluginInterface* getInterface();
    PluginInterface* getInterface(QString sPluginPath);

private:
    static PluginManager            *m_pInstance;
    PluginInterface                 *m_pInterface = nullptr;
    QMap<QString, PluginInterface*> m_mapPluginInterface;
};

#endif // PLUGINMANAGER_H

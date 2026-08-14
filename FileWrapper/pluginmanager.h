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
    ~PluginManager();

    static PluginManager *getInstance();

    void loadAllPlugins();

    PluginInterface* getInterface(QString sPluginPath);

private:
    static PluginManager            *m_pInstance;
    QMap<QString, PluginInterface*> m_mapPluginInterface;
    QMap<QString, QPluginLoader*>   m_mapPluginLoaders;
};

#endif // PLUGINMANAGER_H

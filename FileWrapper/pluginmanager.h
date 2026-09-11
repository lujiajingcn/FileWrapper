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

    // 返回当前已加载的全部插件接口（用于主程序收集插件 widget，避免清理标签页时误删）
    QList<PluginInterface*> getAllInterfaces();

private:
    static PluginManager            *m_pInstance;
    QMap<QString, PluginInterface*> m_mapPluginInterface;
    QMap<QString, QPluginLoader*>   m_mapPluginLoaders;
};

#endif // PLUGINMANAGER_H

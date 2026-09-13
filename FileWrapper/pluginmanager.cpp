#include "pluginmanager.h"
#include <QDir>
#include <QDebug>
#include <QApplication>
#include <QPluginLoader>

PluginManager *PluginManager::m_pInstance = nullptr;

PluginManager::PluginManager(QObject *parent) : QObject (parent)
{
}

PluginManager::~PluginManager()
{
    qDeleteAll(m_mapPluginLoaders);
    m_mapPluginLoaders.clear();
    m_mapPluginInterface.clear();
}

PluginManager *PluginManager::getInstance()
{
    if(m_pInstance == nullptr)
        m_pInstance = new PluginManager;
    return m_pInstance;
}

void PluginManager::loadAllPlugins()
{
    // 已加载过则跳过，避免重复 reload
    if (!m_mapPluginLoaders.isEmpty())
        return;

    QDir pluginsDir(qApp->applicationDirPath());
    if (!pluginsDir.cd("plugins"))
    {
        qWarning() << "[Plugin] 插件目录不存在:" << pluginsDir.absoluteFilePath("plugins");
        return;
    }

    // 只处理真正的动态库文件。
    // MinGW 在生成 PluginXxx.dll 的同时会在同目录留下导入库 libPluginXxx.a，
    // 若不过滤，这些 .a 会被逐个当作插件尝试加载并刷出一堆
    // "Failed to extract plugin meta data from ...libPluginXxx.a" 报错。
#ifdef Q_OS_WIN
    QStringList sFilters;
    sFilters << "*.dll";
#elif defined(Q_OS_MAC)
    QStringList sFilters;
    sFilters << "*.dylib" << "*.so";
#else
    QStringList sFilters;
    sFilters << "*.so";
#endif

    foreach (QString fileName, pluginsDir.entryList(sFilters, QDir::Files))
    {
        QString sFilePath = pluginsDir.absoluteFilePath(fileName);
        QPluginLoader *loader = new QPluginLoader(sFilePath, this);
        QObject *plugin = loader->instance();
        if (plugin)
        {
            PluginInterface *iface = qobject_cast<PluginInterface *>(plugin);
            if (iface)
            {
                m_mapPluginInterface[sFilePath] = iface;
                m_mapPluginLoaders[sFilePath] = loader;
                qDebug() << "[Plugin] Loaded:" << sFilePath;
            }
            else
            {
                qWarning() << "[Plugin] Cast failed (not a PluginInterface):" << sFilePath;
                delete loader;
            }
        }
        else
        {
            qWarning() << "[Plugin] Load failed:" << sFilePath << "-" << loader->errorString();
            delete loader;
        }
    }
}

PluginInterface* PluginManager::getInterface(QString sPluginPath)
{
    QMap<QString, PluginInterface*>::const_iterator cIt = m_mapPluginInterface.find(sPluginPath);
    if(cIt == m_mapPluginInterface.end())
        return nullptr;
    return cIt.value();
}

QList<PluginInterface*> PluginManager::getAllInterfaces()
{
    return m_mapPluginInterface.values();
}

#include "pluginmanager.h"
#include <QDir>
#include <QDebug>
#include <QApplication>
#include <QLibrary>
#include <QPluginLoader>

PluginManager *PluginManager::m_pInstance = nullptr;

PluginManager::PluginManager(QObject *parent) : QObject (parent)
{
}

PluginManager::~PluginManager()
{
    qDeleteAll(m_mapPluginLoaders);
}

PluginManager *PluginManager::getInstance()
{
    if(m_pInstance == nullptr)
        m_pInstance = new PluginManager;
    return m_pInstance;
}

bool PluginManager::loadPlugin()
{
    // 原始实现使用栈上的 QPluginLoader，函数返回时 DLL 被卸载，
    // m_pInterface 变成悬空指针。已改为委托给 loadAllPlugins()。
    loadAllPlugins();
    return m_pInterface != nullptr;
}

void PluginManager::loadAllPlugins()
{
    // 已加载过则跳过，避免重复 reload
    if (!m_mapPluginLoaders.isEmpty())
        return;

    QDir pluginsDir(qApp->applicationDirPath());
    pluginsDir.cd("plugins");
    foreach (QString fileName, pluginsDir.entryList(QDir::Files))
    {
        QString sFilePath = pluginsDir.absoluteFilePath(fileName);
        QPluginLoader *loader = new QPluginLoader(sFilePath, this);
        QObject *plugin = loader->instance();
        if (plugin)
        {
            scanMetaData(sFilePath);
            PluginInterface *iface = qobject_cast<PluginInterface *>(plugin);
            if (iface)
            {
                m_mapPluginInterface[sFilePath] = iface;
                m_mapPluginLoaders[sFilePath] = loader;
            }
            else
            {
                delete loader;
            }
        }
        else
        {
            delete loader;
        }
    }
}


void PluginManager::scanMetaData(const QString &sPath)
{
    // 判断是否为库（后缀有效性）
    if(!QLibrary::isLibrary(sPath))
        return;

    QPluginLoader* loader = new QPluginLoader(sPath);
    QJsonObject jObj = loader->metaData().value("MetaData").toObject();
    for(int i = 0; i < jObj.keys().size(); ++i)
    {
//        qDebug()<<jObj.keys().at(i)<< " : "<<jObj.value(jObj.keys().at(i)) << endl;
    }

    delete loader;
    loader = nullptr;
}

PluginInterface* PluginManager::getInterface()
{
    return m_pInterface;
}

PluginInterface* PluginManager::getInterface(QString sPluginPath)
{
    QMap<QString, PluginInterface*>::const_iterator cIt = m_mapPluginInterface.find(sPluginPath);
    if(cIt == m_mapPluginInterface.end())
        return nullptr;
    return cIt.value();
}

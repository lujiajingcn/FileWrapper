#include "pluginmanager.h"
#include <QDir>
#include <QDebug>
#include <QApplication>

PluginManager *PluginManager::m_pInstance = nullptr;

PluginManager::PluginManager(QObject *parent) : QObject (parent)
{
}

PluginManager *PluginManager::getInstance()
{
    if(m_pInstance == nullptr)
        m_pInstance = new PluginManager;
    return m_pInstance;
}

bool PluginManager::loadPlugin()
{
    QDir pluginsDir(qApp->applicationDirPath());
    pluginsDir.cd("plugins");
    foreach (QString fileName, pluginsDir.entryList(QDir::Files))
    {
        QPluginLoader pluginLoader(pluginsDir.absoluteFilePath(fileName));
        QObject *plugin = pluginLoader.instance();
        if (plugin)
        {
            scanMetaData(pluginsDir.absoluteFilePath(fileName));
            m_pInterface = qobject_cast<PluginInterface *>(plugin);
            if (m_pInterface)
                return true;
        }
    }
    return false;
}

void PluginManager::loadAllPlugins()
{
    QDir pluginsDir(qApp->applicationDirPath());
    pluginsDir.cd("plugins");
    foreach (QString fileName, pluginsDir.entryList(QDir::Files))
    {
        QString sFilePath = pluginsDir.absoluteFilePath(fileName);
        QPluginLoader pluginLoader(sFilePath);
        QObject *plugin = pluginLoader.instance();
        if (plugin)
        {
            scanMetaData(pluginsDir.absoluteFilePath(fileName));
            m_pInterface = qobject_cast<PluginInterface *>(plugin);
            if (m_pInterface)
            {
                m_mapPluginInterface[sFilePath] = m_pInterface;
            }
        }
        else
        {
            continue;
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

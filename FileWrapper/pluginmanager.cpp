#include "pluginmanager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSettings>
#include <QLibrary>

namespace {

// DLL 绝对路径 -> 插件逻辑名（去掉目录、扩展名与 MinGW 的 lib 前缀）
QString pluginIdFromPath(const QString &sPath)
{
    QString sId = QFileInfo(sPath).completeBaseName();
    if (sId.startsWith(QStringLiteral("lib"), Qt::CaseInsensitive))
        sId.remove(0, 3);
    return sId;
}

// 与「插件映射」共用同一个配置文件：%APPDATA%\FileWrapper\FileWrapper.ini
QSettings pluginSettings()
{
    return QSettings(QSettings::IniFormat, QSettings::UserScope,
                     QStringLiteral("FileWrapper"), QStringLiteral("FileWrapper"));
}

const char *kEnabledGroup = "PluginEnabled";

} // namespace
PluginManager *PluginManager::m_pInstance = nullptr;

PluginManager::PluginManager(QObject *parent) : QObject (parent)
{
}

PluginManager::~PluginManager()
{
    qDeleteAll(m_mapPluginLoaders);
    m_mapPluginLoaders.clear();
    m_mapPluginInterface.clear();
    qDeleteAll(m_listRejectedLoaders);
    m_listRejectedLoaders.clear();
}

PluginManager *PluginManager::getInstance()
{
    if(m_pInstance == nullptr)
        m_pInstance = new PluginManager;
    return m_pInstance;
}

// ==================== 私有工具 ====================

void PluginManager::recordFailure(const QString &sPluginPath, const QString &sError)
{
    Failure f;
    f.path  = sPluginPath;
    f.error = sError;
    m_listFailures.append(f);
    qWarning() << "[Plugin] Load failed:" << sPluginPath << "-" << sError;
}

void PluginManager::parseMetaJson(const QJsonObject &pluginJson, const QString &sPluginPath,
                                  PluginMeta *meta)
{
    meta->path         = sPluginPath;
    meta->displayName  = QFileInfo(sPluginPath).fileName();
    meta->declaredName = pluginJson.value(QStringLiteral("Name")).toString();
    meta->apiVersion   = pluginJson.value(QStringLiteral("apiVersion")).toInt();
    meta->extensionPriority = pluginJson.value(QStringLiteral("extensionPriority")).toInt();

    meta->extensions.clear();
    const QJsonArray arr = pluginJson.value(QStringLiteral("extensions")).toArray();
    for (const QJsonValue &v : arr)
    {
        QString sExt = v.toString().trimmed().toLower();
        while (sExt.startsWith(QLatin1Char('.'))) sExt.remove(0, 1);
        if (!sExt.isEmpty() && !meta->extensions.contains(sExt))
            meta->extensions.append(sExt);
    }
}

bool PluginManager::apiVersionAcceptable(const QJsonObject &loaderMetaData, QString *error,
                                        int *foundVersion)
{
    if (foundVersion) *foundVersion = 0;
    if (error) error->clear();

    const QJsonObject json = loaderMetaData.value(QStringLiteral("MetaData")).toObject();
    if (json.isEmpty())
    {
        if (error)
            *error = QStringLiteral("插件元数据为空：不是 FileWrapper 插件，或文件已损坏");
        return false;
    }

    const QJsonValue v = json.value(QStringLiteral("apiVersion"));
    if (!v.isDouble())
    {
        if (error)
            *error = QStringLiteral(
                "插件元数据里没有 apiVersion —— 这是引入版本协商之前的旧插件。\n"
                "接口的虚函数表已变化，直接加载会崩溃，因此拒绝加载；"
                "请用当前源码重新构建插件（PLUGIN_API_VERSION=%1）。")
                .arg(PLUGIN_API_VERSION);
        return false;
    }

    const int found = v.toInt();
    if (foundVersion) *foundVersion = found;
    if (found != PLUGIN_API_VERSION)
    {
        if (error)
            *error = QStringLiteral(
                "插件 API 版本为 %1，当前程序要求 %2 —— 插件与主程序不是同一版本，"
                "不能混用。请用同一套源码重新构建插件。")
                .arg(found).arg(PLUGIN_API_VERSION);
        return false;
    }
    return true;
}

bool PluginManager::tryLoadPlugin(const QString &sPluginPath, QString *error,
                                  QPluginLoader **outLoader, PluginInterface **outIface,
                                  PluginMeta *outMeta)
{
    if (outLoader) *outLoader = nullptr;
    if (outIface)  *outIface  = nullptr;
    if (error)     error->clear();

    QPluginLoader *loader = new QPluginLoader(sPluginPath, this);

    // 1) 先读元数据（不实例化插件）。读不到说明 DLL 本身加载失败（缺依赖等）。
    const QJsonObject md = loader->metaData();
    if (md.isEmpty())
    {
        if (error) *error = loader->errorString();
        delete loader;                       // 从未实例化，可安全卸载
        return false;
    }

    // 2) B1：版本门禁 —— 必须在实例化之前，跨版本调用虚函数是 UB
    QString sVerErr;
    int nFound = 0;
    if (!apiVersionAcceptable(md, &sVerErr, &nFound))
    {
        if (error) *error = sVerErr;
        delete loader;
        return false;
    }

    // 3) 实例化 + 接口转换
    QObject *plugin = loader->instance();
    if (plugin == nullptr)
    {
        if (error) *error = loader->errorString();
        delete loader;
        return false;
    }

    PluginInterface *iface = qobject_cast<PluginInterface *>(plugin);
    if (iface == nullptr)
    {
        if (error)
            *error = QStringLiteral("可以加载，但不是 FileWrapper 插件接口"
                                    "（缺少 Q_INTERFACES，或接口 IID 不匹配）");
        m_listRejectedLoaders.append(loader);   // 对象已创建：不能卸载 DLL
        return false;
    }

    // 4) 复核插件自报版本（防止元数据与实际代码不一致）
    const int nRealVersion = iface->apiVersion();
    if (nRealVersion != PLUGIN_API_VERSION)
    {
        if (error)
            *error = QStringLiteral("插件自报 API 版本 %1，与程序要求（%2）不一致 —— "
                                    "元数据与实际代码不同步，拒绝使用")
                         .arg(nRealVersion).arg(PLUGIN_API_VERSION);
        m_listRejectedLoaders.append(loader);
        return false;
    }

    PluginMeta meta;
    parseMetaJson(md.value(QStringLiteral("MetaData")).toObject(), sPluginPath, &meta);
    meta.apiVersion = nRealVersion;

    if (outLoader) *outLoader = loader;
    if (outIface)  *outIface  = iface;
    if (outMeta)   *outMeta   = meta;
    return true;
}

void PluginManager::registerExtensions(const PluginMeta &meta)
{
    for (const QString &sExt : meta.extensions)
    {
        auto it = m_mapExtToPath.find(sExt);
        if (it == m_mapExtToPath.end())
        {
            m_mapExtToPath.insert(sExt, meta.path);
            continue;
        }
        if (it.value() == meta.path)
            continue;                       // 同一插件（sidecar + 内嵌）重复登记，忽略

        // 后缀冲突：按 extensionPriority 决出，大者优先；相同优先级保留先注册的。
        // 目前唯一已知冲突是 ".ts"（MPEG-TS 视频 vs TypeScript 文本），
        // 历史行为是文本优先 —— PluginText 在 JSON 里声明 extensionPriority=10。
        const int nExisting = m_mapPluginMeta.value(it.value()).extensionPriority;
        if (meta.extensionPriority > nExisting)
        {
            qWarning() << "[Plugin] 后缀" << sExt << "被多个插件声明，按 extensionPriority"
                       << "改由" << meta.path << "处理（原:" << it.value()
                       << "priority" << nExisting << "->" << meta.extensionPriority << "）";
            it.value() = meta.path;
        }
        else
        {
            qWarning() << "[Plugin] 后缀" << sExt << "被多个插件声明，保留" << it.value()
                       << "(priority" << nExisting << ")，忽略" << meta.path;
        }
    }
}

void PluginManager::registerPlugin(const QString &sPluginPath, PluginInterface *iface,
                                   QPluginLoader *loader, const PluginMeta &meta)
{
    m_mapPluginInterface[sPluginPath] = iface;
    m_mapPluginLoaders[sPluginPath]   = loader;
    m_mapPluginMeta[sPluginPath]      = meta;
    registerExtensions(meta);
}

void PluginManager::scanSidecarMetadata()
{
    QDir pluginsDir(qApp->applicationDirPath());
    if (!pluginsDir.cd(QStringLiteral("plugins")))
        return;

    // sidecar 与 DLL 同名（PluginWord.dll <-> PluginWord.json），先建基名索引
    QMap<QString, QString> mapBaseToDll;
    foreach (const QString &sFile, pluginsDir.entryList(QDir::Files))
    {
        if (!QLibrary::isLibrary(sFile))
            continue;
        mapBaseToDll.insert(QFileInfo(sFile).completeBaseName().toLower(),
                            pluginsDir.absoluteFilePath(sFile));
    }

    foreach (const QString &sJson, pluginsDir.entryList(QStringList() << QStringLiteral("*.json"),
                                                        QDir::Files))
    {
        const QString sDllPath = mapBaseToDll.value(QFileInfo(sJson).completeBaseName().toLower());
        if (sDllPath.isEmpty())
            continue;                       // 没有同名 DLL：不是插件元数据

        QFile f(pluginsDir.absoluteFilePath(sJson));
        if (!f.open(QIODevice::ReadOnly))
            continue;
        QJsonParseError parseErr;
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseErr);
        if (!doc.isObject())
        {
            qWarning() << "[Plugin] sidecar 元数据不是合法 JSON:" << sJson
                       << parseErr.errorString();
            continue;
        }

        PluginMeta meta;
        parseMetaJson(doc.object(), sDllPath, &meta);
        // 加载成功时随后会用内嵌元数据覆盖；这里先登记，保证"加载失败也有路由信息"
        if (!m_mapPluginMeta.contains(sDllPath))
            m_mapPluginMeta.insert(sDllPath, meta);
        registerExtensions(meta);
    }
}

// ================= C5: 启用/禁用 + 重新加载 =================

bool PluginManager::isPluginEnabled(const QString &sPluginId)
{
    if (sPluginId.isEmpty())
        return true;
    QSettings st = pluginSettings();
    st.beginGroup(QString::fromLatin1(kEnabledGroup));
    return st.value(sPluginId, true).toBool();
}

void PluginManager::setPluginEnabled(const QString &sPluginId, bool bEnabled)
{
    if (sPluginId.isEmpty())
        return;
    QSettings st = pluginSettings();
    st.beginGroup(QString::fromLatin1(kEnabledGroup));
    if (bEnabled) st.remove(sPluginId);      // 默认就是启用，不写冗余值
    else          st.setValue(sPluginId, false);
    st.sync();
}

void PluginManager::reloadPlugins()
{
    // 先释放所有 loader（= 卸载 DLL）。调用方必须保证没有标签页仍持有插件实例，
    // 否则那些对象的虚表会指向已卸载的模块。
    qDeleteAll(m_mapPluginLoaders);
    m_mapPluginLoaders.clear();
    m_mapPluginInterface.clear();
    qDeleteAll(m_listRejectedLoaders);
    m_listRejectedLoaders.clear();

    m_mapPluginMeta.clear();
    m_mapExtToPath.clear();
    m_listFailures.clear();
    m_listDisabled.clear();

    loadAllPlugins();
}

// ================= 加载 =================

void PluginManager::loadAllPlugins()
{
    // 已加载过则跳过，避免重复 reload
    if (!m_mapPluginLoaders.isEmpty())
        return;

    m_listFailures.clear();
    m_listDisabled.clear();
    m_mapPluginMeta.clear();
    m_mapExtToPath.clear();

    QDir pluginsDir(qApp->applicationDirPath());
    if (!pluginsDir.cd(QStringLiteral("plugins")))
    {
        qWarning() << "[Plugin] 插件目录不存在:" << pluginsDir.absoluteFilePath("plugins");
        return;
    }

    // 先按 sidecar 登记"谁声明了哪些后缀"（含稍后加载失败的插件）
    scanSidecarMetadata();

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

    foreach (const QString &sFileName, pluginsDir.entryList(sFilters, QDir::Files))
    {
        const QString sFilePath = pluginsDir.absoluteFilePath(sFileName);

        // C5：用户禁用的插件不加载（但 sidecar 的"后缀归谁"仍然登记，
        // 这样打开该格式时会提示"插件已被禁用，可在插件管理里启用"，
        // 而不是笼统的"未找到插件"）。
        const QString sPluginId = pluginIdFromPath(sFilePath);
        if (!isPluginEnabled(sPluginId))
        {
            m_listDisabled.append(sPluginId);
            qDebug() << "[Plugin] Disabled:" << sPluginId << sFilePath;
            continue;
        }

        QString sError;
        QPluginLoader *loader = nullptr;
        PluginInterface *iface = nullptr;
        PluginMeta meta;
        if (!tryLoadPlugin(sFilePath, &sError, &loader, &iface, &meta))
        {
            recordFailure(sFilePath, sError);
            continue;
        }

        registerPlugin(sFilePath, iface, loader, meta);
        qDebug() << "[Plugin] Loaded:" << sFilePath
                 << "api=" << meta.apiVersion
                 << "exts=" << meta.extensions.size();
    }
}

// ==================== 查询 ====================

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

QString PluginManager::pluginForExtension(const QString &sExt) const
{
    QString sKey = sExt.trimmed().toLower();
    while (sKey.startsWith(QLatin1Char('.'))) sKey.remove(0, 1);
    return m_mapExtToPath.value(sKey);
}

QString PluginManager::pluginPathForId(const QString &sId) const
{
    QString sKey = sId.trimmed();
    // 允许用户/映射表里带扩展名
    static const QStringList kLibSuffixes = {
        QStringLiteral(".dll"), QStringLiteral(".so"),
        QStringLiteral(".dylib"), QStringLiteral(".a")
    };
    for (const QString &suf : kLibSuffixes)
        if (sKey.endsWith(suf, Qt::CaseInsensitive))
        {
            sKey.chop(suf.size());
            break;
        }
    if (sKey.isEmpty())
        return QString();

    for (auto it = m_mapPluginMeta.constBegin(); it != m_mapPluginMeta.constEnd(); ++it)
    {
        const QString sBase = QFileInfo(it.value().path).completeBaseName();
        QString sBaseNoLib = sBase;
        if (sBaseNoLib.startsWith(QStringLiteral("lib"), Qt::CaseInsensitive))
            sBaseNoLib.remove(0, 3);

        if (sBase.compare(sKey, Qt::CaseInsensitive) == 0
            || sBaseNoLib.compare(sKey, Qt::CaseInsensitive) == 0
            || it.value().declaredName.compare(sKey, Qt::CaseInsensitive) == 0)
            return it.value().path;
    }
    return QString();
}

QStringList PluginManager::knownPluginIds() const
{
    QStringList ids;
    for (auto it = m_mapPluginMeta.constBegin(); it != m_mapPluginMeta.constEnd(); ++it)
    {
        QString sBase = QFileInfo(it.value().path).completeBaseName();
        if (sBase.startsWith(QStringLiteral("lib"), Qt::CaseInsensitive))
            sBase.remove(0, 3);
        if (!sBase.isEmpty() && !ids.contains(sBase, Qt::CaseInsensitive))
            ids.append(sBase);
    }
    ids.sort(Qt::CaseInsensitive);
    return ids;
}

QStringList PluginManager::extensionsOf(const QString &sPluginPath) const
{
    const auto it = m_mapPluginMeta.constFind(sPluginPath);
    return it == m_mapPluginMeta.constEnd() ? QStringList() : it.value().extensions;
}

PluginInterface *PluginManager::createInstanceFor(PluginInterface *templ, QString *error)
{
    if (error) error->clear();
    if (templ == nullptr)
    {
        if (error) *error = QStringLiteral("插件模板为空");
        return nullptr;
    }

    PluginInterface *pInstance = templ->createInstance();
    if (pInstance == nullptr && error)
    {
        *error = QStringLiteral(
            "插件没有实现 createInstance()（返回空）。每个标签页都需要独立实例，"
            "共享实例会让多个标签页互相覆盖内容，因此这里不允许退化。");
    }
    return pInstance;
}

QString PluginManager::failureFor(const QString &sPluginPath) const
{
    for (const Failure &f : m_listFailures)
        if (f.path == sPluginPath)
            return f.error;
    return QString();
}

// ==================== 重试 ====================

int PluginManager::retryFailedPlugins()
{
    if (m_listFailures.isEmpty())
        return 0;

    int nRecovered = 0;
    QList<Failure> listStillFailed;

    const QList<Failure> listSnapshot = m_listFailures;
    for (const Failure &f : listSnapshot)
    {
        if (m_mapPluginInterface.contains(f.path))
            continue;                       // 期间已经成功过

        // 用户可能已经把它禁用了：禁用不是"失败"，重试也不应该把它加载回来
        if (!isPluginEnabled(pluginIdFromPath(f.path)))
        {
            qDebug() << "[Plugin] Skip retry (disabled):" << f.path;
            continue;
        }

        QString sError;
        QPluginLoader *loader = nullptr;
        PluginInterface *iface = nullptr;
        PluginMeta meta;
        if (tryLoadPlugin(f.path, &sError, &loader, &iface, &meta))
        {
            registerPlugin(f.path, iface, loader, meta);
            ++nRecovered;
            qDebug() << "[Plugin] Recovered:" << f.path;
            continue;
        }
        listStillFailed.append(Failure{ f.path, sError });
    }

    m_listFailures = listStillFailed;
    if (nRecovered > 0)
        qDebug() << "[Plugin] 重试加载成功" << nRecovered << "个插件";
    return nRecovered;
}

#ifndef PLUGINMANAGER_H
#define PLUGINMANAGER_H

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPluginLoader>
#include <QString>
#include <QStringList>

#include "plugininterface.h"

// ============================================================================
// 插件管理器
//
// C1（插件自描述后缀）：每个插件在自己的元数据 JSON 里声明 "extensions"，
//      主程序不再维护那张 90 项的后缀表；这里汇总成 ext -> 插件路径。
//      插件 DLL 加载失败时，同目录的 sidecar（plugins/<DLL基名>.json）仍能被读到，
//      所以「哪个后缀归哪个插件」依然清楚 —— 用户打开 .docx 时看到的是
//      「PluginWord 不可用，缺 Qt6Xml.dll」，而不是笼统的「未找到插件」。
// B1（版本协商）：加载前先读插件内嵌元数据里的 "apiVersion" 与 PLUGIN_API_VERSION
//      比对，不匹配就不实例化（跨版本调用虚函数是 UB）；实例化之后再复核一次。
// A2（失败可见）：失败的插件与原始错误都记在 failures() 里，供 UI 展示与重试。
// ============================================================================
class PluginManager : public QObject
{
    Q_OBJECT

public:
    // 加载失败的插件：path = DLL 绝对路径，error = 可读原因
    struct Failure
    {
        QString path;
        QString error;
    };

    // 一个插件的静态信息：内嵌元数据（加载成功时）或 sidecar JSON（加载失败时）
    struct PluginMeta
    {
        QString     path;          // DLL 绝对路径
        QString     displayName;   // 文件名，如 "PluginWord.dll"
        QString     declaredName;  // JSON 里的 "Name"，如 "pluginWord"
        QStringList extensions;    // JSON 里的 "extensions"（已转小写）
        int         apiVersion = 0;
        // 后缀冲突时的优先级（JSON 里的 "extensionPriority"，默认 0，大者优先）。
        // 例：".ts" 既是 MPEG-TS 视频又是 TypeScript 文本，历史行为是文本优先，
        // 所以 PluginText 声明 10，PluginMedia 用默认 0。
        int         extensionPriority = 0;
    };

    explicit PluginManager(QObject *parent = nullptr);
    ~PluginManager();

    static PluginManager *getInstance();

    // P0-3：宿主存在「停播未确认」的插件实例时，禁止卸载/重载 DLL。
    // 进程退出阶段由 MainWindow 调用 false，PluginManager 析构会跳过 unload。
    void setUnloadEnabled(bool bEnabled);
    bool isUnloadEnabled() const { return m_bUnloadEnabled; }

    void loadAllPlugins();

    PluginInterface* getInterface(QString sPluginPath);

    // 返回当前已加载的全部插件接口（用于主程序收集插件 widget，避免清理标签页时误删）
    QList<PluginInterface*> getAllInterfaces();

    // —— C1：后缀路由 ——
    // 返回声明了该后缀的插件路径（大小写不敏感）。**即使该插件加载失败也会返回**，
    // 以便调用方走「插件不可用」诊断，而不是假装"没有插件"。
    QString pluginForExtension(const QString &sExt) const;

    // 逻辑名（DLL 基名 / JSON "Name"，大小写不敏感）-> DLL 绝对路径
    QString pluginPathForId(const QString &sId) const;

    // 已知插件的逻辑名（含加载失败的；来自 sidecar），供「插件映射」下拉框使用
    QStringList knownPluginIds() const;

    QStringList extensionsOf(const QString &sPluginPath) const;
    int registeredExtensionCount() const { return m_mapExtToPath.size(); }

    // —— B1：版本协商（纯函数，便于单测）——
    // 入参是 QPluginLoader::metaData() 的完整结果（里面 "MetaData" 才是插件 JSON）。
    static bool apiVersionAcceptable(const QJsonObject &loaderMetaData, QString *error,
                                     int *foundVersion = nullptr);

    // —— B2：标签页实例 ——
    // 为一个标签页创建插件实例。失败返回 nullptr 并给出可读原因。
    // **不要**在任何地方退回"共享模板实例"：那会让多个标签页互相覆盖内容，
    // 且只有一行 qWarning 能看出异常（历史上就是这么写的，已废弃）。
    static PluginInterface *createInstanceFor(PluginInterface *templ, QString *error);

    QList<Failure> failures() const { return m_listFailures; }
    QString failureFor(const QString &sPluginPath) const;

    // —— C5：插件启用/禁用 + 重新加载 ——
    // 禁用状态持久化在 QSettings 的 PluginEnabled 组里（键 = 插件逻辑名）。
    // 被禁用的插件不会加载、也不参与后缀路由（用户打开该格式会得到"未找到插件"
    // 的内置兜底，而不是"插件不可用"的报错）。
    static bool isPluginEnabled(const QString &sPluginId);
    static void setPluginEnabled(const QString &sPluginId, bool bEnabled);
    QStringList disabledPluginIds() const { return m_listDisabled; }

    // 重新扫描 plugins/ 目录：会**卸载**已加载的插件 DLL。
    // 调用者必须保证没有任何标签页仍持有插件实例（否则对象虚表悬空）。
    void reloadPlugins();

    // 重新尝试加载此前失败的插件。**不卸载已加载成功的插件**：
    // 正在显示的标签页里持有其实例，卸载 DLL 会让那些指针悬空。
    // 返回本次成功恢复的插件数量。
    int retryFailedPlugins();

private:
    void recordFailure(const QString &sPluginPath, const QString &sError);

    // 尝试加载一个插件 DLL：读元数据 -> 版本门禁 -> 实例化 -> 接口转换 -> 版本复核。
    // 失败时 error 填原因；只有"从未实例化"的情况才会卸载/销毁 loader。
    bool tryLoadPlugin(const QString &sPluginPath, QString *error,
                       QPluginLoader **outLoader, PluginInterface **outIface,
                       PluginMeta *outMeta);

    void registerPlugin(const QString &sPluginPath, PluginInterface *iface,
                        QPluginLoader *loader, const PluginMeta &meta);
    void registerExtensions(const PluginMeta &meta);

    // 扫描 plugins/*.json（与 DLL 同名的 sidecar），让加载失败的插件也能贡献路由信息
    void scanSidecarMetadata();

    // 从插件 JSON 里取 Name / extensions / apiVersion
    static void parseMetaJson(const QJsonObject &pluginJson, const QString &sPluginPath,
                              PluginMeta *meta);

    static PluginManager            *m_pInstance;
    QMap<QString, PluginInterface*> m_mapPluginInterface;   // path -> 接口
    QMap<QString, QPluginLoader*>   m_mapPluginLoaders;     // path -> loader
    QMap<QString, PluginMeta>       m_mapPluginMeta;        // path -> 元数据
    QMap<QString, QString>          m_mapExtToPath;         // 小写后缀 -> path
    QList<Failure>                  m_listFailures;
    QStringList                     m_listDisabled;        // 本次扫描中被禁用的插件逻辑名

    // 已加载成功但被拒（接口不符 / 版本自查不符）的 loader：对象已创建，
    // 销毁 loader 会尝试卸载 DLL，导致对象虚表悬空 —— 只能留着。
    QList<QPluginLoader *>          m_listRejectedLoaders;
    bool                            m_bUnloadEnabled = true;   // P0-3
};

#endif // PLUGINMANAGER_H

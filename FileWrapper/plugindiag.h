#ifndef PLUGINDIAG_H
#define PLUGINDIAG_H

#include <QList>
#include <QString>
#include <QStringList>

// ============================================================================
// 插件运行环境自检（2026-11-17 引入）
//
// 背景：插件依赖缺失时 QPluginLoader 只会给出「找不到指定的模块」这种看不出缺什么
// 的报错，而主程序此前只在 qDebug 里记一行 —— 用户双击文件后毫无反应。这里把构建期
// 生成的「部署清单」(plugin-deploy-manifest.txt) 与实际文件比对，得出缺哪些依赖、
// 每一项是干什么用的，再与 Qt 的原始错误一起组成可读报告，供「插件不可用」对话框和
// 「插件设置」的「状态与依赖」页使用。
//
// 清单由 deploy/deploy-deps.ps1 生成，数据源是 deploy/plugin-deps.manifest；
// 清单每行格式：<相对程序目录的路径>|<用途说明>
// ============================================================================
class PluginDiagnostics
{
public:
    struct MissingFile
    {
        QString path;   // 相对程序目录的路径，例如 "Qt6Xml.dll"
        QString hint;   // 用途说明（可能为空）
    };

    static QString manifestName();

    // 部署清单的完整路径
    static QString manifestPath(const QString &appDir);

    // 与部署清单逐项比对，返回缺失的文件；清单不存在或读不出来时 error 非空、返回空
    static QList<MissingFile> missingFiles(const QString &appDir, QString *error = nullptr);

    // 组装「插件加载失败 + 缺什么 + 怎么办」的纯文本报告（对话框直接显示）
    // alsoFailedPlugins：同时加载失败的其它插件文件名（可为空）
    static QString buildReport(const QString &pluginPath, const QString &pluginError,
                               const QString &appDir,
                               const QStringList &alsoFailedPlugins = QStringList());

    // 一行摘要，供状态栏与「插件设置」抬头使用
    static QString summaryLine(const QString &appDir, int failedPlugins);

    // 文件名（含扩展名）/ 逻辑名（去扩展名、去掉 MinGW 的 lib 前缀）
    static QString pluginDisplayName(const QString &pluginPath);
    static QString pluginId(const QString &pluginPath);
};

#endif // PLUGINDIAG_H

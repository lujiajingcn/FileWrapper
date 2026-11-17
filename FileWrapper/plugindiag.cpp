#include "plugindiag.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringConverter>
#include <QTextStream>

// ----------------------------------------------------------------------------
// 部署清单
// ----------------------------------------------------------------------------
QString PluginDiagnostics::manifestName()
{
    return QStringLiteral("plugin-deploy-manifest.txt");
}

QString PluginDiagnostics::manifestPath(const QString &appDir)
{
    return QDir(appDir).absoluteFilePath(manifestName());
}

QList<PluginDiagnostics::MissingFile> PluginDiagnostics::missingFiles(const QString &appDir,
                                                                     QString *error)
{
    if (error) error->clear();

    QList<MissingFile> missing;
    const QString sPath = manifestPath(appDir);

    QFile f(sPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        if (error)
            *error = QStringLiteral("没有找到部署清单：%1\n"
                                    "（说明构建时没有执行依赖部署步骤，插件依赖无法自检）")
                         .arg(sPath);
        return missing;
    }

    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);

    while (!in.atEnd())
    {
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QStringLiteral("//")))
            continue;

        const int bar = line.indexOf(QLatin1Char('|'));
        MissingFile item;
        if (bar < 0)
        {
            item.path = line;
        }
        else
        {
            item.path = line.left(bar).trimmed();
            item.hint = line.mid(bar + 1).trimmed();
        }
        if (item.path.isEmpty())
            continue;

        // 清单里的路径统一用 '/' 分隔，QDir::filePath 在 Windows 上也能正确拼接
        const QString sAbs = QDir(appDir).filePath(item.path);
        if (!QFileInfo::exists(sAbs))
        {
            item.path.replace(QLatin1Char('/'), QDir::separator());
            missing.append(item);
        }
    }
    return missing;
}

// ----------------------------------------------------------------------------
// 报告
// ----------------------------------------------------------------------------
QString PluginDiagnostics::buildReport(const QString &pluginPath, const QString &pluginError,
                                       const QString &appDir,
                                       const QStringList &alsoFailedPlugins)
{
    QStringList lines;

    lines << QStringLiteral("插件文件：%1").arg(pluginPath);
    if (pluginError.trimmed().isEmpty())
        lines << QStringLiteral("错误信息：本次会话没有记录到该插件的加载错误"
                                "（文件可能被删除/改名，或插件目录不可读）");
    else
        lines << QStringLiteral("错误信息：%1").arg(pluginError.trimmed());

    if (!alsoFailedPlugins.isEmpty())
        lines << QStringLiteral("同时加载失败的插件：%1").arg(alsoFailedPlugins.join(
                    QStringLiteral("、")));

    lines << QString();

    QString sCheckError;
    const QList<MissingFile> missing = missingFiles(appDir, &sCheckError);
    if (!sCheckError.isEmpty())
    {
        lines << QStringLiteral("部署自检：%1").arg(sCheckError);
    }
    else if (missing.isEmpty())
    {
        lines << QStringLiteral("部署自检：部署清单里的依赖都在。"
                                "那么更可能是构建环境不一致 —— 请确认插件与本程序"
                                "用的是同一套 Qt 与编译器，且同为 debug 或同为 release。");
    }
    else
    {
        lines << QStringLiteral("部署自检：缺少 %1 个依赖文件").arg(missing.size());
        for (const MissingFile &m : missing)
        {
            if (m.hint.isEmpty())
                lines << QStringLiteral("  · %1").arg(m.path);
            else
                lines << QStringLiteral("  · %1 —— %2").arg(m.path, m.hint);
        }
        lines << QString();
        lines << QStringLiteral("处理办法：在工程目录执行一次完整构建（mingw32-make），"
                                "或只补部署（mingw32-make deploydeps），"
                                "也可以从 Qt 安装目录 / FFmpeg 发行包里把上面列出的文件"
                                "复制到程序目录，然后点「重试加载」。");
    }

    return lines.join(QLatin1Char('\n'));
}

QString PluginDiagnostics::summaryLine(const QString &appDir, int failedPlugins)
{
    QString sCheckError;
    const QList<MissingFile> missing = missingFiles(appDir, &sCheckError);

    QStringList parts;
    if (failedPlugins > 0)
        parts << QStringLiteral("插件加载失败 %1 个").arg(failedPlugins);
    if (!sCheckError.isEmpty())
        parts << QStringLiteral("无部署清单");
    else if (!missing.isEmpty())
        parts << QStringLiteral("依赖缺失 %1 项").arg(missing.size());
    else
        parts << QStringLiteral("依赖齐全");

    return parts.join(QStringLiteral(" · "));
}

// ----------------------------------------------------------------------------
// 命名
// ----------------------------------------------------------------------------
QString PluginDiagnostics::pluginDisplayName(const QString &pluginPath)
{
    const QString sName = QFileInfo(pluginPath).fileName();
    return sName.isEmpty() ? pluginPath : sName;
}

QString PluginDiagnostics::pluginId(const QString &pluginPath)
{
    QString sId = QFileInfo(pluginPath).completeBaseName();
    // MinGW 产出的导入库/某些平台命名带 lib 前缀，与用户映射里保存的名字对齐
    if (sId.startsWith(QStringLiteral("lib"), Qt::CaseInsensitive))
        sId.remove(0, 3);
    return sId;
}

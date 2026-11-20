#ifndef FW_ICONS_H
#define FW_ICONS_H

#include <QIcon>
#include <QSize>
#include <QString>

// 图标资源由 tools/icons/build-iconset.mjs 生成：字形取自 IconMCP（MIT），
// 同一套线性图标按 16/18/20/24/32/40/48/64 px 各渲一份，避免 Qt 从小图放大。
namespace FwIcons {
inline QIcon get(const QString &name)
{
    QIcon icon;
    for (int s : {16, 18, 20, 24, 32, 40, 48, 64})
        icon.addFile(QStringLiteral(":/icons/%1_%2.png").arg(name).arg(s), QSize(s, s));
    return icon;
}

// 窗口/任务栏图标额外带 128/256 px（高 DPI 与 Alt+Tab 用大图更清晰）。
inline QIcon appIcon()
{
    QIcon icon;
    for (int s : {16, 20, 24, 32, 40, 48, 64, 128, 256})
        icon.addFile(QStringLiteral(":/icons/appicon_%1.png").arg(s), QSize(s, s));
    return icon;
}
} // namespace FwIcons

#endif // FW_ICONS_H

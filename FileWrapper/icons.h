#ifndef FW_ICONS_H
#define FW_ICONS_H

#include <QIcon>
#include <QSize>
#include <QString>

namespace FwIcons {
inline QIcon get(const QString &name)
{
    QIcon icon;
    for (int s : {20, 24, 32, 40, 48, 64})
        icon.addFile(QStringLiteral(":/icons/%1_%2.png").arg(name).arg(s), QSize(s, s));
    return icon;
}
} // namespace FwIcons

#endif // FW_ICONS_H

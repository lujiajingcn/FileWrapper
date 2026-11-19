#include "filemetadata.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace {

#ifdef Q_OS_WIN
LPCWSTR winPath(const QString &sPath)
{
    return reinterpret_cast<LPCWSTR>(sPath.utf16());
}
#endif

// QDateTime -> UTC 毫秒；无效或超出可显示范围时返回 0（未知）。
qint64 toTimestampMs(const QDateTime &dt)
{
    if (!dt.isValid())
        return 0;
    const qint64 ms = dt.toUTC().toMSecsSinceEpoch();
    return FwMeta::isPlausibleTimestamp(ms) ? ms : 0;
}

quint32 readAttributes(const QString &sPath, const QFileInfo &fi)
{
#ifdef Q_OS_WIN
    const DWORD nAttrs = GetFileAttributesW(winPath(sPath));
    if (nAttrs != INVALID_FILE_ATTRIBUTES)
        return quint32(nAttrs) & FwMeta::ATTR_MASK;
#endif
    quint32 nFallbackAttrs = 0;
    if (!fi.isWritable())
        nFallbackAttrs |= FwMeta::ATTR_READONLY;
    return nFallbackAttrs;
}

} // namespace

FwMeta::Metadata FwMeta::capture(const QString &sPath)
{
    Metadata m;
    const QFileInfo fi(sPath);
    if (!fi.exists() || !fi.isFile())
        return m;

    m.createdMs  = toTimestampMs(fi.birthTime());
    m.modifiedMs = toTimestampMs(fi.lastModified());
    m.accessedMs = toTimestampMs(fi.lastRead());
    m.attributes = readAttributes(fi.absoluteFilePath(), fi);
    m.valid      = true;
    return m;
}

bool FwMeta::apply(const QString &sPath, qint64 createdMs, qint64 modifiedMs,
                   qint64 accessedMs, quint32 attributes, QString *psErr)
{
    auto fail = [psErr](const QString &sMsg) {
        if (psErr) *psErr = sMsg;
        return false;
    };
    if (psErr) psErr->clear();

    const QFileInfo fi(sPath);
    if (!fi.exists() || !fi.isFile())
        return fail(QStringLiteral("目标文件不存在，无法写入元数据：%1").arg(sPath));

    const bool bHasCreated  = isPlausibleTimestamp(createdMs);
    const bool bHasModified = isPlausibleTimestamp(modifiedMs);
    const bool bHasAccessed = isPlausibleTimestamp(accessedMs);
    if (!bHasCreated && !bHasModified && !bHasAccessed && attributes == 0)
        return true;   // 没有可恢复的元数据
    const bool bHasTime = bHasCreated || bHasModified || bHasAccessed;

#ifdef Q_OS_WIN
    // 只读会阻止后续写时间/属性，先临时清掉；如果最终没有可写的属性记录，
    // 或中途失败，要把它恢复回去（只读是用户的文件属性，不能被我们悄悄去掉）。
    const DWORD nOriginalAttrs = GetFileAttributesW(winPath(sPath));
    const bool bClearedReadOnly =
        nOriginalAttrs != INVALID_FILE_ATTRIBUTES
        && (nOriginalAttrs & FILE_ATTRIBUTE_READONLY)
        && SetFileAttributesW(winPath(sPath), nOriginalAttrs & ~DWORD(FILE_ATTRIBUTE_READONLY));
#endif
    const auto restore = [&]() {
#ifdef Q_OS_WIN
        if (bClearedReadOnly)
            SetFileAttributesW(winPath(sPath), nOriginalAttrs);
#endif
    };

    if (bHasTime)
    {
        // QFileDevice::setFileTime() 是实例方法：先打开文件，再逐项写。
        // 不能用只读方式打开（Windows 上会拒绝写时间）。
        QFile f(sPath);
        if (!f.open(QIODevice::ReadWrite))
        {
            restore();
            return fail(QStringLiteral("打开输出文件失败（无法写入时间）：%1").arg(sPath));
        }

        bool bAnyTimeSet = false;
        QString sFirstErr;
        auto setOne = [&](qint64 nMs, QFileDevice::FileTime eTime, const char *szName) {
            if (!isPlausibleTimestamp(nMs))
                return;
            if (f.setFileTime(QDateTime::fromMSecsSinceEpoch(nMs), eTime))
                bAnyTimeSet = true;
            else if (sFirstErr.isEmpty())
                sFirstErr = QStringLiteral("写入%1失败：%2").arg(QString::fromLatin1(szName), sPath);
        };
        setOne(createdMs,  QFileDevice::FileBirthTime,        "创建时间");
        setOne(modifiedMs, QFileDevice::FileModificationTime, "修改时间");
        // 访问时间最后写：前面的打开/写入动作可能刷新它
        setOne(accessedMs, QFileDevice::FileAccessTime,       "访问时间");
        f.close();

        if (!bAnyTimeSet && !sFirstErr.isEmpty())
        {
            restore();
            return fail(sFirstErr);
        }
    }

    if (attributes == 0)
    {
        restore();   // 归档未记录属性：保持磁盘现状
        return true;
    }

#ifdef Q_OS_WIN
    DWORD nNow = GetFileAttributesW(winPath(sPath));
    if (nNow == INVALID_FILE_ATTRIBUTES)
    {
        restore();
        return fail(QStringLiteral("读取文件属性失败：%1").arg(sPath));
    }
    nNow = (nNow & ~DWORD(ATTR_MASK)) | (DWORD(attributes) & DWORD(ATTR_MASK));
    if (!SetFileAttributesW(winPath(sPath), nNow))
    {
        restore();
        return fail(QStringLiteral("写入文件属性失败：%1").arg(sPath));
    }
#else
    QFile::Permissions perms = QFileInfo(sPath).permissions();
    if (attributes & ATTR_READONLY)
        perms &= ~(QFile::WriteOwner | QFile::WriteUser | QFile::WriteGroup | QFile::WriteOther);
    else
        perms |= QFile::WriteOwner | QFile::WriteUser;
    if (!QFile::setPermissions(sPath, perms))
        return fail(QStringLiteral("写入文件属性失败：%1").arg(sPath));
#endif
    return true;
}

QString FwMeta::attributesText(quint32 attributes)
{
    const quint32 nAttrs = attributes & ATTR_MASK;
    QString sText;
    if (nAttrs & ATTR_READONLY) sText += QLatin1Char('R');
    if (nAttrs & ATTR_HIDDEN)   sText += QLatin1Char('H');
    if (nAttrs & ATTR_SYSTEM)   sText += QLatin1Char('S');
    if (nAttrs & ATTR_ARCHIVE)  sText += QLatin1Char('A');
    return sText.isEmpty() ? QStringLiteral("—") : sText;
}

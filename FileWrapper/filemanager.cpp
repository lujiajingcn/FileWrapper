#include "filemanager.h"
#include "filemetadata.h"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>
#include <QDateTime>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QSet>
#include <algorithm>
#include <cstring>
#include <new>

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#define MAXBUFFERSIZE (1024 * 1024 * 16)
static const qint64 kCopyChunk = 1024 * 1024;      // 流式复制缓冲 1 MB
static const qint64 kCrcChunk  = 1024 * 1024;      // 头部区校验缓冲
// 进度事件最多每 100 ms 泵一次：文件 I/O 期间仍能刷新进度/响应取消，
// 但把 processEvents() 带来的重入窗口压到原来的十分之一（P1-1）。
static const qint64 kProgressPumpIntervalMs = 100;

namespace {

// P0-1：本次操作创建并独占的辅助文件。只清理由操作自己登记的文件，
// 绝不预先删除一个「按固定规则推导出来、可能属于别人」的路径。
struct OwnedAuxFiles
{
    QStringList paths;

    void add(const QString &sPath)
    {
        if (!sPath.isEmpty() && !paths.contains(sPath))
            paths.append(sPath);
    }

    void release(const QString &sPath) { paths.removeAll(sPath); }

    void removeNow(const QString &sPath)
    {
        if (sPath.isEmpty()) return;
        QFile::remove(sPath);
        paths.removeAll(sPath);
    }

    ~OwnedAuxFiles()
    {
        for (const QString &sPath : paths)
            if (!sPath.isEmpty())
                QFile::remove(sPath);
    }
};

QString randomToken()
{
    return QString::number(QRandomGenerator::global()->generate64(), 16)
           + QStringLiteral("-")
           + QString::number(QCoreApplication::applicationPid());
}

// 路径是否指向同一个文件（绝对路径 + canonical 双比较，兼容大小写/相对路径）
bool sameFilePath(const QString &a, const QString &b)
{
    if (a.isEmpty() || b.isEmpty()) return false;
    const QString aCanon = QFileInfo(a).canonicalFilePath();
    const QString bCanon = QFileInfo(b).canonicalFilePath();
    if (!aCanon.isEmpty() && !bCanon.isEmpty()
        && aCanon.compare(bCanon, Qt::CaseInsensitive) == 0)
        return true;
    return QFileInfo(a).absoluteFilePath().compare(
               QFileInfo(b).absoluteFilePath(), Qt::CaseInsensitive) == 0;
}

// 在目标目录中以「随机名 + NewOnly」原子创建临时文件，返回路径。
// NewOnly 保证不会复用/截断一个已存在的路径（并发进程也抢不走同一个名字）。
bool createUniqueTempFile(const QString &sTargetPath, QString &sTmpPath, QString &sErr)
{
    const QFileInfo fi(sTargetPath);
    const QDir dir(fi.absolutePath());
    if (!dir.exists())
    {
        sErr = QObject::tr("输出目录不存在：%1").arg(dir.absolutePath());
        return false;
    }

    for (int i = 0; i < 16; ++i)
    {
        const QString sCandidate = dir.filePath(
            QStringLiteral(".fwda-tmp-%1-%2").arg(randomToken()).arg(i));
        QFile probe(sCandidate);
        if (probe.open(QIODevice::ReadWrite | QIODevice::NewOnly))
        {
            probe.close();
            sTmpPath = sCandidate;
            return true;
        }
    }
    sErr = QObject::tr("无法在输出目录创建唯一的临时文件：%1").arg(dir.absolutePath());
    return false;
}

// 把现有目标文件移动到一个唯一备份名（随机 + NewOnly 语义）。
// 不预先删除任何固定路径；失败时给出具体原因。
bool moveToUniqueBackup(const QString &sTargetPath, QString &sBakPath, QString &sErr)
{
    for (int i = 0; i < 16; ++i)
    {
        const QString sCandidate = sTargetPath
            + QStringLiteral(".bak-%1-%2").arg(randomToken()).arg(i);
        if (QFileInfo::exists(sCandidate))
            continue;
        if (QFile::rename(sTargetPath, sCandidate))
        {
            sBakPath = sCandidate;
            return true;
        }
        if (!QFileInfo::exists(sTargetPath))
        {
            sErr = QObject::tr("原文件在替换前已不存在：%1").arg(sTargetPath);
            return false;
        }
    }
    sErr = QObject::tr("无法为 %1 生成唯一的备份文件名（目录写入受限？）").arg(sTargetPath);
    return false;
}

// 回滚：把备份换回目标路径。目标（新文件）先挪到唯一临时名再删除，
// 避免「删新文件失败 → 备份也换不回去」的两难；失败时返回可照做的绝对路径。
bool restoreFromBackup(const QString &sTargetPath, const QString &sBakPath,
                       QString &sRollbackErr)
{
    QString sReplaced;
    if (QFileInfo::exists(sTargetPath))
    {
        for (int i = 0; i < 8; ++i)
        {
            sReplaced = sTargetPath + QStringLiteral(".failed-%1-%2")
                            .arg(randomToken()).arg(i);
            if (QFileInfo::exists(sReplaced)) continue;
            if (QFile::rename(sTargetPath, sReplaced)) break;
            sReplaced.clear();
        }
        if (sReplaced.isEmpty())
        {
            sRollbackErr = QObject::tr("无法移走损坏的新文件：%1").arg(sTargetPath);
            return false;
        }
    }

    if (!QFile::rename(sBakPath, sTargetPath))
    {
        // 换不回去：至少把损坏的新文件放回原位并保留备份，用户仍可手动恢复
        if (!sReplaced.isEmpty() && !QFileInfo::exists(sTargetPath))
            QFile::rename(sReplaced, sTargetPath);
        sRollbackErr = QObject::tr("无法把备份 %1 换回 %2")
                           .arg(sBakPath, sTargetPath);
        return false;
    }

    if (!sReplaced.isEmpty())
        QFile::remove(sReplaced);
    return true;
}

// 锁文件里记录的 PID 是否仍存活。30 秒只是“崩溃残留”的判定起点，不能
// 单独作为“可以删锁”的依据：一次几 GB 的保存完全可能超过 30 秒，旧实现
// 会让第二个实例把仍在写的锁删掉，两个进程同时替换同一个输出。
bool isProcessAliveForLock(qint64 nPid)
{
    if (nPid <= 0)
        return false;
#ifdef Q_OS_WIN
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                           FALSE, DWORD(nPid));
    if (h == nullptr)
    {
        // 打不开不等于已退出：权限不足时保守地当成“仍在运行”，避免误删活锁。
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    DWORD nExit = 0;
    const bool bAlive = GetExitCodeProcess(h, &nExit) && nExit == STILL_ACTIVE;
    CloseHandle(h);
    return bAlive;
#else
    return QFileInfo::exists(QStringLiteral("/proc/%1").arg(nPid));
#endif
}

// P0-1：同一输出文件的跨进程写锁。
//
// 不用 QLockFile：它在受限权限/沙箱环境里会因 CreateFile 的附加权限请求失败
// （QLockFile::PermissionError），把正常的保存全部挡死。这里用最保守的
// 「NewOnly 原子创建 + 内容写 PID + 30 秒陈旧判定」实现同等语义：
//   * 创建成功 = 拿到锁；
//   * 已存在且未超过 30 秒 = 另一个实例正在写；
//   * 超过 30 秒视为崩溃残留，删除后重试一次；
//   * 环境连锁文件都建不了时按无锁继续（不能因为锁而让用户无法保存）。
class OutputWriteLock
{
public:
    bool acquire(const QString &sTargetPath, QString &sErr)
    {
        m_path = QFileInfo(sTargetPath).absoluteFilePath() + QStringLiteral(".fwdalock");
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            QFile f(m_path);
            if (f.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            {
                f.write(QByteArray::number(qint64(QCoreApplication::applicationPid())));
                f.close();
                m_locked = true;
                return true;
            }
            if (!QFileInfo::exists(m_path))
            {
                qWarning() << "[FileManager] 无法创建输出锁文件，按无锁继续:" << m_path
                           << f.errorString();
                return true;
            }

            const QFileInfo fi(m_path);
            qint64 nLockPid = 0;
            {
                QFile lockReader(m_path);
                if (lockReader.open(QIODevice::ReadOnly))
                    nLockPid = lockReader.read(64).trimmed().toLongLong();
            }
            const qint64 nAgeMs = fi.lastModified().isValid()
                ? fi.lastModified().msecsTo(QDateTime::currentDateTime())
                : 0;
            // 只有“锁已超过陈旧阈值”且“记录的进程确实不在了”时才删锁。
            // 长保存期间不刷新 mtime 也没关系：PID 仍存活就必须拒绝第二个写者。
            if (nAgeMs > 30000 && !isProcessAliveForLock(nLockPid))
            {
                qWarning() << "[FileManager] 清理崩溃残留的写锁:" << m_path
                           << "pid=" << nLockPid << "ageMs=" << nAgeMs;
                QFile::remove(m_path);
                continue;
            }
            sErr = QObject::tr("另一个 FileWrapper 实例正在写入该文件，请稍后重试：%1")
                       .arg(sTargetPath);
            return false;
        }
        sErr = QObject::tr("无法获取输出写锁（锁文件已存在）：%1").arg(sTargetPath);
        return false;
    }

    ~OutputWriteLock()
    {
        if (!m_locked)
            return;
        // 只删除仍属于自己的锁：若另一个实例清理过陈旧锁并重新建锁，
        // 无条件 remove 会把它的锁也删掉，导致后续写入者无保护。
        QFile f(m_path);
        bool bOurs = false;
        if (f.open(QIODevice::ReadOnly))
            bOurs = f.read(64).trimmed().toLongLong()
                    == qint64(QCoreApplication::applicationPid());
        f.close();
        if (bOurs)
            QFile::remove(m_path);
    }

private:
    QString m_path;
    bool    m_locked = false;
};

} // namespace


// 「按原路径」时把归档 displayPath 还原为绝对路径：
//   - 拒绝 "." / ".." 段，避免导出/分割写出归档指定目录之外；
//   - 跨盘/rootless 归档把 E:/x 存成 E/x，这里补回盘符冒号；
//   - 非绝对路径直接拒绝，而不是按当前工作目录静默落盘。
static QString absoluteOriginalPath(const QString &sDisplayPath, QString *psErr = nullptr)
{
    if (psErr) psErr->clear();
    QString p = FwArc::normalizePath(sDisplayPath);
    const QStringList segs = p.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (segs.isEmpty())
    {
        if (psErr) *psErr = QStringLiteral("路径为空");
        return QString();
    }
    for (const QString &seg : segs)
    {
        if (seg == QLatin1String(".") || seg == QLatin1String(".."))
        {
            if (psErr) *psErr = QStringLiteral("路径包含非法回溯段：%1").arg(sDisplayPath);
            return QString();
        }
    }

    // 扁平模式：E:/x 会存成 E/x（无冒号）。仅单个字母首段才还原，避免误伤普通相对目录。
    const QChar chDrive = segs.isEmpty() ? QChar() : segs.first().at(0);
    const bool bAsciiDrive = segs.size() >= 2 && segs.first().size() == 1
        && ((chDrive >= QLatin1Char('A') && chDrive <= QLatin1Char('Z'))
            || (chDrive >= QLatin1Char('a') && chDrive <= QLatin1Char('z')));
    if (bAsciiDrive)
    {
        QStringList rest = segs;
        const QString sDrive = rest.takeFirst();
        p = sDrive + QStringLiteral(":/") + rest.join(QLatin1Char('/'));
    }

    const QFileInfo fi(p);
    if (!fi.isAbsolute())
    {
        if (psErr) *psErr = QStringLiteral("不是绝对路径：%1").arg(sDisplayPath);
        return QString();
    }
    return fi.absoluteFilePath();
}

// ============================================================
// 基础访问器
// ============================================================

FileManager::FileManager(QObject *parent) : QObject(parent)
{
}

void FileManager::setError(const QString &sMsg)
{
    m_sLastError = sMsg;
    qWarning() << "[FileManager]" << sMsg;
    emit warningOccurred(sMsg);
}

QString FileManager::lastError() const { return m_sLastError; }
QString FileManager::lastOutputPath() const { return m_sLastOutputPath; }
bool    FileManager::isBusy() const { return m_nBusyDepth > 0; }
bool    FileManager::isCancelRequested() const { return m_bCancelRequested; }
bool    FileManager::isArchiveLoaded() const
{
    return m_qFile.isOpen() && !m_qFile.fileName().isEmpty();
}
QString FileManager::archivePath() const
{
    return isArchiveLoaded() ? m_qFile.fileName() : QString();
}
bool FileManager::isDirty() const { return m_bDirty; }
bool FileManager::needsSave() const
{
    return isArchiveLoaded() && m_bDirty;
}
FwArc::Meta FileManager::archiveMeta() const { return m_meta; }
QVector<FILEINFO> FileManager::getFileInfos() { return m_vtFileInfos; }

QString FileManager::getFileName(QString sFilePath)
{
    const int nPos = qMax(sFilePath.lastIndexOf('/'), sFilePath.lastIndexOf('\\'));
    return sFilePath.right(sFilePath.length() - (nPos + 1));
}

// ============================================================
// 统一解析入口（P1-1 / P2-3）
//
// 头部 CRC、条目字段、区间越界/重叠、UTF-8 与路径安全全部在这里校验。
// validateMergedFile / QLoadMergedFile / QSplitFiles / verifyArchive 共用。
// ============================================================

bool FileManager::parseFromDevice(QFile &f, FwArc::Meta &meta, QString &sErr)
{
    auto fail = [&sErr](const QString &m) { sErr = m; return false; };

    meta = FwArc::Meta();
    if (!f.isOpen()) return fail(QStringLiteral("归档文件未打开"));

    const quint64 fileSize = quint64(f.size());
    meta.fileSize = fileSize;
    if (fileSize < FwArc::GLOBAL_HDR)
        return fail(QStringLiteral("文件过小，不是有效的归档"));

    if (!f.seek(0)) return fail(QStringLiteral("定位文件头失败"));
    char gh[FwArc::GLOBAL_HDR];
    if (f.read(gh, FwArc::GLOBAL_HDR) != FwArc::GLOBAL_HDR)
        return fail(QStringLiteral("读取全局头失败"));

    if (std::memcmp(gh, FwArc::MAGIC, 4) != 0)
        return fail(QStringLiteral("魔数不匹配，不是有效的归档"));

    const quint32 version   = FwArc::readU32(gh + 4);
    const quint32 hdrSize   = FwArc::readU32(gh + 8);
    const quint32 fileCount = FwArc::readU32(gh + 12);
    const quint32 storedCrc = FwArc::readU32(gh + 16);

    if (version != FwArc::CURRENT_VERSION)
        return fail(QStringLiteral("不支持的归档版本: %1（当前只支持 %2）")
                        .arg(version).arg(FwArc::CURRENT_VERSION));
    if (fileCount == 0 || fileCount > FwArc::MAX_FILES)
        return fail(QStringLiteral("文件数量异常: %1").arg(fileCount));
    if (hdrSize < FwArc::GLOBAL_HDR || quint64(hdrSize) > fileSize
        || quint64(hdrSize) > FwArc::MAX_HDR_SIZE)
        return fail(QStringLiteral("头部大小异常: %1").arg(hdrSize));

    const quint32 entryFixed = FwArc::ENTRY_FIXED;
    const quint64 entriesRegion = quint64(hdrSize) - FwArc::GLOBAL_HDR;
    if (entriesRegion < 4 + quint64(entryFixed) * fileCount)
        return fail(QStringLiteral("头部大小与文件数量不匹配"));

    // —— 头部 CRC：分块计算，不一次性分配头部大小的内存（P3-2）——
    FwArc::Crc32 crc;
    crc.update(gh, 16);
    crc.update(gh + 20, 4);
    if (!f.seek(FwArc::GLOBAL_HDR)) return fail(QStringLiteral("定位头部区失败"));
    QByteArray chunk;
    chunk.resize(int(kCrcChunk));
    quint64 remain = entriesRegion;
    while (remain > 0)
    {
        const qint64 n = qMin<quint64>(remain, kCrcChunk);
        const qint64 got = f.read(chunk.data(), n);
        if (got != n) return fail(QStringLiteral("头部区读取不完整"));
        crc.update(chunk.data(), got);
        remain -= quint64(got);
    }
    if (crc.value() != storedCrc)
        return fail(QStringLiteral("头部校验失败（CRC32 不匹配）"));

    meta.version    = version;
    meta.headerSize = hdrSize;
    meta.fileCount  = fileCount;

    // —— 逐条解析：流式读取，不缓存整个头部区 ——
    if (!f.seek(FwArc::GLOBAL_HDR)) return fail(QStringLiteral("定位条目区失败"));
    qint64 cursor = FwArc::GLOBAL_HDR;

    char rl[4];
    if (f.read(rl, 4) != 4) return fail(QStringLiteral("读取原始根长度失败"));
    const quint32 rootLen = FwArc::readU32(rl);
    if (rootLen > FwArc::MAX_ROOT_LEN)
        return fail(QStringLiteral("原始根长度异常: %1").arg(rootLen));
    if (cursor + 4 + rootLen > hdrSize)
        return fail(QStringLiteral("原始根越界"));
    QByteArray rb(qsizetype(rootLen), Qt::Uninitialized);
    if (rootLen && f.read(rb.data(), rootLen) != rootLen)
        return fail(QStringLiteral("读取原始根失败"));
    if (!FwArc::isValidUtf8(rb))
        return fail(QStringLiteral("原始根不是合法 UTF-8"));
    meta.root = QString::fromUtf8(rb);
    cursor += 4 + rootLen;

    meta.entries.reserve(int(fileCount));
    for (quint32 i = 0; i < fileCount; ++i)
    {
        if (cursor + qint64(entryFixed) > hdrSize)
            return fail(QStringLiteral("第 %1 条记录越界（条目区不足）").arg(i));

        char plBuf[4];
        if (f.read(plBuf, 4) != 4) return fail(QStringLiteral("第 %1 条路径长度读取失败").arg(i));
        const quint32 pathLen = FwArc::readU32(plBuf);
        if (pathLen == 0 || pathLen > FwArc::MAX_PATH_LEN)
            return fail(QStringLiteral("第 %1 条路径长度异常: %2").arg(i).arg(pathLen));
        if (cursor + qint64(entryFixed) + pathLen > hdrSize)
            return fail(QStringLiteral("第 %1 条记录越界（路径区不足）").arg(i));

        char fsBuf[8], cpBuf[8];
        if (f.read(fsBuf, 8) != 8) return fail(QStringLiteral("第 %1 条大小读取失败").arg(i));
        if (f.read(cpBuf, 8) != 8) return fail(QStringLiteral("第 %1 条偏移读取失败").arg(i));
        char crcBuf[4];
        if (f.read(crcBuf, 4) != 4) return fail(QStringLiteral("第 %1 条内容 CRC 读取失败").arg(i));
        const quint32 entryCrc = FwArc::readU32(crcBuf);

        // 固定字段里的文件系统元数据（时间 UTC 毫秒 + DOS 属性）
        char metaBuf[8];
        if (f.read(metaBuf, 8) != 8) return fail(QStringLiteral("第 %1 条创建时间读取失败").arg(i));
        const quint64 createdMs = FwArc::readU64(metaBuf);
        if (f.read(metaBuf, 8) != 8) return fail(QStringLiteral("第 %1 条修改时间读取失败").arg(i));
        const quint64 modifiedMs = FwArc::readU64(metaBuf);
        if (f.read(metaBuf, 8) != 8) return fail(QStringLiteral("第 %1 条访问时间读取失败").arg(i));
        const quint64 accessedMs = FwArc::readU64(metaBuf);
        char attrBuf[4];
        if (f.read(attrBuf, 4) != 4) return fail(QStringLiteral("第 %1 条属性读取失败").arg(i));
        const quint32 attributes = FwArc::readU32(attrBuf);

        // 固定字段末尾的 TagCount（标记正文在路径之后，见下面的循环）
        char tagCntBuf[4];
        if (f.read(tagCntBuf, 4) != 4)
            return fail(QStringLiteral("第 %1 条标记数量读取失败").arg(i));
        const quint32 tagCount = FwArc::readU32(tagCntBuf);
        if (tagCount > FwArc::MAX_TAGS)
            return fail(QStringLiteral("第 %1 条标记数量异常: %2").arg(i).arg(tagCount));

        const quint64 size = FwArc::readU64(fsBuf);
        const quint64 pos  = FwArc::readU64(cpBuf);

        QByteArray pb(qsizetype(pathLen), Qt::Uninitialized);
        if (f.read(pb.data(), pathLen) != pathLen)
            return fail(QStringLiteral("第 %1 条路径读取失败").arg(i));
        if (!FwArc::isValidUtf8(pb))
            return fail(QStringLiteral("第 %1 条路径不是合法 UTF-8").arg(i));

        const QString path = QString::fromUtf8(pb);

        // 路径之后的标记区（TagCount 个 [TagLen u32 + UTF-8 正文]）。
        // 边界按「条目起点 + 固定字段 + 路径 + 已读标记字节」计算：cursor 此时
        // 仍停在条目起点，不能直接拿它做当前读位置的边界判断。
        QStringList tags;
        qint64 tagBytes = 0;
        tags.reserve(int(tagCount));
        for (quint32 t = 0; t < tagCount; ++t)
        {
            const qint64 tagPos = cursor + qint64(entryFixed) + qint64(pathLen) + tagBytes;
            if (tagPos + 4 > qint64(hdrSize))
                return fail(QStringLiteral("第 %1 条标记区越界").arg(i));
            char tagLenBuf[4];
            if (f.read(tagLenBuf, 4) != 4)
                return fail(QStringLiteral("第 %1 条标记长度读取失败").arg(i));
            const quint32 tagLen = FwArc::readU32(tagLenBuf);
            if (tagLen == 0 || tagLen > FwArc::MAX_TAG_LEN)
                return fail(QStringLiteral("第 %1 条标记长度异常: %2").arg(i).arg(tagLen));
            if (tagPos + 4 + qint64(tagLen) > qint64(hdrSize))
                return fail(QStringLiteral("第 %1 条标记正文越界").arg(i));
            QByteArray tb(qsizetype(tagLen), Qt::Uninitialized);
            if (f.read(tb.data(), tagLen) != tagLen)
                return fail(QStringLiteral("第 %1 条标记读取失败").arg(i));
            if (!FwArc::isValidUtf8(tb))
                return fail(QStringLiteral("第 %1 条标记不是合法 UTF-8").arg(i));
            const QString tag = QString::fromUtf8(tb);
            QString tagErr;
            if (!FwArc::isValidTag(tag, &tagErr))
                return fail(QStringLiteral("第 %1 条标记非法：%2").arg(i).arg(tagErr));
            tags << tag;
            tagBytes += 4 + qint64(tagLen);
        }

        FwArc::Entry e;
        e.size       = size;
        e.contentPos = pos;
        e.contentCrc = entryCrc;
        e.tags       = tags;
        // 时间/属性按「头部里存什么就显示什么」处理；伪造的超界时间戳
        // 收敛为 0（未知），避免显示无意义的年份。
        e.createdMs  = FwArc::sanitizeTimestamp(createdMs);
        e.modifiedMs = FwArc::sanitizeTimestamp(modifiedMs);
        e.accessedMs = FwArc::sanitizeTimestamp(accessedMs);
        e.attributes = attributes & FwMeta::ATTR_MASK;

        QString why;
        if (!FwArc::isSafeRelativePath(path, &why))
            return fail(QStringLiteral("第 %1 条路径不安全：%2").arg(i).arg(why));
        e.relPath     = path;
        e.displayPath = FwArc::displayPathFor(meta.root, path);

        // —— 条目字段合法性：伪造 fileSize / contentPos 会在这里被拒绝（P1-1）——
        if (size > FwArc::MAX_CONTENT_LEN)
            return fail(QStringLiteral("第 %1 条文件大小异常: %2").arg(i).arg(size));
        if (pos < hdrSize)
            return fail(QStringLiteral("第 %1 条内容偏移落在头部区: %2").arg(i).arg(pos));
        if (pos > fileSize || size > fileSize - pos)
            return fail(QStringLiteral("第 %1 条内容区越界（size=%2 pos=%3 文件=%4）")
                        .arg(i).arg(size).arg(pos).arg(fileSize));

        meta.entries.append(e);
        cursor += qint64(entryFixed) + qint64(pathLen) + tagBytes;
    }

    // 内容区不得重叠。
    //
    // 注意零长度条目（0 字节文件）：写入端 writeArchiveHeader 按文件长度累加内容偏移，
    // 0 字节文件不推进游标，于是「它自己」和「它后面那条」共享同一个 contentPos。
    // 这在语义上是正确的 —— 0 字节文件本来就不占任何内容区。
    //
    // 但若把零长度区间也放进重叠检测列表，排序后 [P, P) 与 [P, P+N) 首地址相同，
    // 而 std::sort 不保证稳定，两者的相对次序是未定义的：
    //   · [P, P)   排在前面 -> P < P     为假 -> 不报错
    //   · [P, P+N) 排在前面 -> P < P+N   为真 -> 误报「内容区重叠」
    // 表现为「目录里有空文件就合并失败」，且随文件数/排序实现而变（本次实测
    // 224 种组合里 22 种触发，报错位置随目录结构漂移）。
    //
    // 因此：零长度条目不参与重叠检测（它不占内容区，本就无从重叠）；
    // 真正的重叠（两个非零长度区间交叠）仍必须被拦截。
    QVector<QPair<quint64, quint64>> ranges;
    ranges.reserve(meta.entries.size());
    for (const FwArc::Entry &e : meta.entries)
    {
        if (e.size == 0) continue;   // 零长度条目不占内容区，不参与重叠判定
        ranges.append(qMakePair(e.contentPos, e.contentPos + e.size));
    }
    std::sort(ranges.begin(), ranges.end(),
              [](const QPair<quint64, quint64> &a, const QPair<quint64, quint64> &b) {
                  return a.first < b.first;
              });
    for (int i = 1; i < ranges.size(); ++i)
        if (ranges.at(i).first < ranges.at(i - 1).second)
            return fail(QStringLiteral("条目内容区重叠（第 %1 条与前一区间冲突）").arg(i));

    return true;
}

bool FileManager::parseFile(const QString &sPath, FwArc::Meta &meta, QString &sErr) const
{
    QFile f(sPath);
    if (!f.open(QIODevice::ReadOnly))
    {
        sErr = QStringLiteral("打开文件失败: %1 (%2)").arg(sPath, f.errorString());
        return false;
    }
    return parseFromDevice(f, meta, sErr);
}

// P1-1：每个操作使用自己的 QFile。以前 entryHead()/entryContent() 都复用 m_qFile，
// 而 streamCopy() 又在循环里 processEvents()；重入时第二次 seek 会把外层复制游标
// 改到别的条目位置，错误字节会被静默写进新归档。
// 独占句柄后，共享游标不再是正确性依赖。
bool FileManager::openEntrySource(const QString &sFilePath, const FILEINFO &fi,
                                  QFile &localFile, qint64 &srcPos, QString &sErr) const
{
    srcPos = 0;
    if (m_setPendingFiles.contains(sFilePath))
    {
        localFile.setFileName(sFilePath);
        if (!localFile.open(QIODevice::ReadOnly))
        {
            sErr = tr("读取新增文件失败：%1 (%2)").arg(sFilePath, localFile.errorString());
            return false;
        }
        // P1-6：新增文件加入归档后被外部改写时，旧实现只复制缓存的旧长度前缀。
        // 长度变化必须明确失败，而不是静默保存一个不完整的条目。
        if (fi.nFileLen >= 0 && localFile.size() != fi.nFileLen)
        {
            sErr = tr("文件在加入归档后已发生变化（加入时 %1 B，当前 %2 B）：%3\n"
                      "请删除该条目后重新添加。")
                       .arg(fi.nFileLen).arg(localFile.size()).arg(sFilePath);
            return false;
        }
        return true;
    }

    const QString sArchive = m_qFile.fileName();
    if (sArchive.isEmpty() || !m_qFile.isOpen())
    {
        sErr = tr("归档文件未打开");
        return false;
    }
    localFile.setFileName(sArchive);
    if (!localFile.open(QIODevice::ReadOnly))
    {
        sErr = tr("打开归档失败：%1 (%2)").arg(sArchive, localFile.errorString());
        return false;
    }
    srcPos = fi.nContentPositon;
    return true;
}

bool FileManager::validateMergedFile(QString sFilePath)
{
    return validateMergedFile(sFilePath, static_cast<QString *>(nullptr));
}

bool FileManager::validateMergedFile(QString sFilePath, QString *psErrMsg)
{
    FwArc::Meta meta;
    QString err;
    const bool ok = parseFile(sFilePath, meta, err);
    if (psErrMsg) *psErrMsg = ok ? QString() : err;
    if (!ok) setError(QStringLiteral("归档校验失败: %1").arg(err));
    return ok;
}

// P1-9：校验与加载共用一次解析结果
bool FileManager::validateMergedFile(QString sFilePath, FwArc::Meta *pMeta, QString *psErrMsg)
{
    FwArc::Meta meta;
    QString err;
    const bool ok = parseFile(sFilePath, meta, err);
    if (ok && pMeta != nullptr) *pMeta = meta;
    if (psErrMsg) *psErrMsg = ok ? QString() : err;
    if (!ok) setError(QStringLiteral("归档校验失败: %1").arg(err));
    return ok;
}

// B4：临时副本写入前的磁盘空间预检。
// 大条目（几十 GB）复制到临时目录时，如果空间不够，以前要写到一半才失败，
// 只留一句系统错误；这里先算清楚再把可读结论交给调用方。
bool FileManager::hasFreeSpaceFor(const QString &dirPath, qint64 needBytes, qint64 *pAvailable)
{
    if (pAvailable) *pAvailable = -1;

    const QString sPath = dirPath.isEmpty() ? QDir::tempPath() : dirPath;
    const QStorageInfo si(sPath);
    if (!si.isValid() || !si.isReady())
        return true;                        // 拿不到磁盘信息：不拦，让写入错误兜底

    const qint64 nAvail = si.bytesAvailable();
    if (pAvailable) *pAvailable = nAvail;
    if (needBytes <= 0)
        return true;
    return nAvail >= needBytes;
}

bool FileManager::inspectArchive(const QString &sPath, FwArc::Meta &meta, QString &sErr)
{
    QFile f(sPath);
    if (!f.open(QIODevice::ReadOnly))
    {
        sErr = QStringLiteral("打开文件失败: %1 (%2)").arg(sPath, f.errorString());
        return false;
    }
    return parseFromDevice(f, meta, sErr);
}

bool FileManager::extractEntryToTempFile(const QString &sDisplayName,
                                         const QString &sOutPath, QString &sErr)
{
    // 大文件抽取也可能被取消；开始前复位，避免上一次取消把这次直接判掉
    m_bCancelRequested = false;
    auto it = m_mapFileInfos.find(sDisplayName);
    if (it == m_mapFileInfos.end())
    {
        sErr = QStringLiteral("归档内未找到文件: %1").arg(sDisplayName);
        return false;
    }
    const FILEINFO fi = it.value();

    QFile fOut(sOutPath);
    if (!fOut.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        sErr = QStringLiteral("创建临时文件失败: %1 (%2)").arg(sOutPath, fOut.errorString());
        return false;
    }
    // 打开成功后的任何提前 return 都要删掉已创建的空/半成品临时文件。
    struct TempFileGuard {
        QString path;
        bool keep = false;
        ~TempFileGuard() { if (!keep) QFile::remove(path); }
    } tempGuard{sOutPath};

    // P1-1：本操作独占的源句柄（不使用共享的 m_qFile）
    QFile localSrc;
    qint64 srcPos = 0;
    if (!openEntrySource(sDisplayName, fi, localSrc, srcPos, sErr))
    {
        QFile::remove(sOutPath);
        return false;
    }

    FwArc::Crc32 crc;
    qint64 copiedTotal = 0;
    const bool bOk = streamCopy(localSrc, srcPos, fi.nFileLen, fOut, crc,
                                copiedTotal, qMax<qint64>(fi.nFileLen, 1),
                                QStringLiteral("正在准备: ") + fi.sFileName, sErr);
    localSrc.close();
    fOut.close();
    if (!bOk)
    {
        QFile::remove(sOutPath);
        if (sErr.isEmpty()) sErr = QStringLiteral("抽取文件内容失败");
        return false;
    }
    if (m_mapContentCrc.contains(sDisplayName) && crc.value() != m_mapContentCrc.value(sDisplayName))
    {
        QFile::remove(sOutPath);
        sErr = QStringLiteral("内容校验失败（CRC32 不匹配）: %1").arg(sDisplayName);
        return false;
    }
    tempGuard.keep = true;
    return true;
}

// ============================================================
// 加载 / 卸载 / 状态重置（P1-5 / P1-6）
// ============================================================

bool FileManager::QLoadMergedFile(QString sFilePath, const FwArc::Meta *pPreParsed)
{
    // 先在临时结构上做完整解析；任何失败都保持原归档、原视图、原脏状态不变。
    // P1-9：调用方已经 validate 过时直接复用解析结果，避免头部与条目区解析两遍。
    FwArc::Meta meta;
    QString err;
    if (pPreParsed != nullptr)
    {
        meta = *pPreParsed;
        // 预解析结果必须仍对应当前文件（两次调用之间文件可能被改写）
        if (QFileInfo(sFilePath).size() != qint64(meta.fileSize))
        {
            setError(QStringLiteral("文件在解析后发生变化，已拒绝加载：%1").arg(sFilePath));
            return false;
        }
    }
    else if (!parseFile(sFilePath, meta, err))
    {
        setError(QStringLiteral("加载失败: %1").arg(err));
        return false;
    }
    // 没有条目的归档对查看器没有意义；必须在切换句柄/清空旧状态之前拒绝，
    // 否则会出现"提示原有归档保持不变，实际旧内容已被清空"的矛盾状态。
    if (meta.entries.isEmpty())
    {
        setError(QStringLiteral("归档中没有可显示的条目: %1").arg(sFilePath));
        return false;
    }
    // 先确认新文件能打开，避免先关旧句柄后失败导致"原归档打不开也不见了"
    {
        QFile probe(sFilePath);
        if (!probe.open(QIODevice::ReadOnly))
        {
            setError(QStringLiteral("打开归档失败: %1 (%2)").arg(sFilePath, probe.errorString()));
            return false;
        }
    }

    // 重复 displayPath 会让 QMap 单值覆盖、两行显示同一内容，删一条后幸存条目
    // 还会变成“归档内未找到”。在切换句柄前直接拒绝整个归档。
    {
        QSet<QString> seenPaths;
        for (const FwArc::Entry &e : meta.entries)
        {
            const QString key = e.displayPath.toLower();
            if (seenPaths.contains(key))
            {
                setError(QStringLiteral("归档包含重复路径，拒绝加载：%1").arg(e.displayPath));
                return false;
            }
            seenPaths.insert(key);
        }
    }

    // 解析成功后才切换句柄与内存状态
    const QString sOldPath = m_qFile.fileName();
    const bool bOldWasOpen = m_qFile.isOpen();
    if (m_qFile.isOpen()) m_qFile.close();
    m_qFile.setFileName(sFilePath);
    if (!m_qFile.open(QIODevice::ReadOnly))
    {
        // 回滚到旧句柄，保持"旧归档仍可浏览"的承诺
        if (bOldWasOpen && !sOldPath.isEmpty())
        {
            m_qFile.setFileName(sOldPath);
            m_qFile.open(QIODevice::ReadOnly);
        }
        setError(QStringLiteral("打开归档失败: %1 (%2)").arg(sFilePath, m_qFile.errorString()));
        return false;
    }

    m_vtFileInfos.clear();
    m_mapFileInfos.clear();
    m_mapContentCrc.clear();
    for (const FwArc::Entry &e : meta.entries)
    {
        FILEINFO fi;
        fi.sFilePath        = e.displayPath;
        fi.sFileName        = getFileName(e.displayPath);
        fi.nFileLen         = qint64(e.size);
        fi.nContentPositon  = qint64(e.contentPos);
        fi.nState           = 0;
        // 三项时间与属性全为 0 时视为「没有可显示的元数据」（源文件当时没有
        // 可用的文件系统信息），属性页会明确写"未记录"。
        const bool bHasMeta = e.createdMs != 0 || e.modifiedMs != 0
                              || e.accessedMs != 0 || e.attributes != 0;
        fi.bHasFsMetadata   = bHasMeta;
        fi.nCreatedTime     = qint64(e.createdMs);
        fi.nModifiedTime    = qint64(e.modifiedMs);
        fi.nAccessedTime    = qint64(e.accessedMs);
        fi.nAttributes      = e.attributes;
        fi.listTags         = e.tags;
        m_vtFileInfos.push_back(fi);
        m_mapFileInfos[fi.sFilePath] = fi;
        m_mapContentCrc.insert(fi.sFilePath, e.contentCrc);
    }

    m_meta              = meta;
    m_setPendingFiles.clear();
    m_bDirty            = false;
    m_bCancelRequested  = false;
    m_sLastError.clear();
    return true;
}

void FileManager::unLoadMergedFile()
{
    if (m_qFile.isOpen()) m_qFile.close();
    m_qFile.setFileName(QString());
    m_vtFileInfos.clear();
    m_mapFileInfos.clear();
    m_setPendingFiles.clear();
    m_mapContentCrc.clear();
    m_meta = FwArc::Meta();
    m_bDirty           = false;
    m_bCancelRequested = false;
    m_sLastError.clear();
}

// ============================================================
// 增删 / 保存
// ============================================================

void FileManager::addFiles(const QVector<QString> &vtNewFiles)
{
    if (!isArchiveLoaded() || isBusy()) return;

    // 保存时会把“绝对路径”压成归档内的存储路径；根为空的扁平归档还会把盘符
    // 写成路径首段（E:/x -> E/x）。添加时只比较原始绝对路径，会让“再次添加
    // 一个已在扁平归档里的文件”成功进入列表，直到保存才报重复路径。
    // 这里用与保存一致的规范化键做一次前置去重。
    auto addKey = [](const QString &p) {
        // 归档里的 rootless 显示路径（如 "E/x"）没有冒号，不能再按当前工作目录
        // 解析成绝对路径；只有真正的绝对路径才做 absoluteFilePath 归一化。
        const QString sAbs = FwArc::isAbsolutePath(p)
                                 ? QFileInfo(p).absoluteFilePath() : p;
        QString s = FwArc::normalizePath(sAbs);
        if (s.size() >= 2 && s.at(1) == QLatin1Char(':') && s.at(0).isLetter())
            s = s.left(1) + s.mid(2);
        return s.toLower();
    };
    QSet<QString> setExistingKeys;
    for (const FILEINFO &info : m_vtFileInfos)
        setExistingKeys.insert(addKey(info.sFilePath));

    bool bChanged = false;
    for (const QString &fp : vtNewFiles)
    {
        QFileInfo fi(fp);
        if (!fi.exists() || !fi.isFile()) continue;
        if (m_mapFileInfos.contains(fp)) continue;
        if (setExistingKeys.contains(addKey(fp))) continue;
        if (quint64(fi.size()) > FwArc::MAX_CONTENT_LEN) continue;

        FILEINFO info;
        info.sFilePath       = fp;
        info.sFileName       = getFileName(fp);
        info.nFileLen        = fi.size();
        info.nContentPositon = 0;
        info.nState          = 0;
        // 添加时先记下源文件的时间/属性：保存时会把它们写进归档。
        // 未保存前属性页也直接显示这份信息。
        const FwMeta::Metadata m = FwMeta::capture(fp);
        if (m.valid)
        {
            info.bHasFsMetadata = true;
            info.nCreatedTime   = m.createdMs;
            info.nModifiedTime  = m.modifiedMs;
            info.nAccessedTime  = m.accessedMs;
            info.nAttributes    = m.attributes;
        }
        m_vtFileInfos.append(info);
        m_mapFileInfos[fp] = info;
        m_setPendingFiles.insert(fp);
        bChanged = true;
    }
    if (bChanged) m_bDirty = true;
}

bool FileManager::deleteFile(const QString &sFilePath)
{
    if (isBusy()) return false;
    int idx = -1;
    for (int i = 0; i < m_vtFileInfos.size(); ++i)
        if (m_vtFileInfos.at(i).sFilePath == sFilePath) { idx = i; break; }
    if (idx < 0) return false;

    m_vtFileInfos.remove(idx);
    m_mapFileInfos.remove(sFilePath);
    m_setPendingFiles.remove(sFilePath);
    m_mapContentCrc.remove(sFilePath);   // 删除后重加同路径文件不能再用旧 CRC
    m_bDirty = true;
    return true;
}

// 设置条目标记。归一化/校验全部在 FwArc 里做，UI 与写归档端共用同一套规则。
// 注意 m_vtFileInfos、m_mapFileInfos、m_meta.entries 三处必须同步，否则
// 属性页（读 FILEINFO）、entryHead（读 map）、保存（读 vector）会各说各话。
bool FileManager::setEntryTags(const QString &sFilePath, const QStringList &listTags,
                               QString *psErr)
{
    if (psErr) psErr->clear();
    if (!isArchiveLoaded())
    {
        if (psErr) *psErr = tr("没有已加载的归档");
        return false;
    }

    QString sWhy;
    const QStringList tags = FwArc::normalizeTags(listTags, &sWhy);
    if (!sWhy.isEmpty())
    {
        if (psErr) *psErr = sWhy;
        return false;
    }

    int idx = -1;
    for (int i = 0; i < m_vtFileInfos.size(); ++i)
        if (m_vtFileInfos.at(i).sFilePath == sFilePath) { idx = i; break; }
    if (idx < 0)
    {
        if (psErr) *psErr = tr("归档内未找到文件：%1").arg(sFilePath);
        return false;
    }
    if (m_vtFileInfos.at(idx).listTags == tags)
        return true;   // 内容没变：不置脏，避免"什么都没改也要保存"

    m_vtFileInfos[idx].listTags = tags;
    auto itMap = m_mapFileInfos.find(sFilePath);
    if (itMap != m_mapFileInfos.end())
        itMap.value().listTags = tags;
    // 让 archiveMeta()（属性/测试/诊断用）与 FILEINFO 保持一致；保存后会被重新解析覆盖。
    for (FwArc::Entry &e : m_meta.entries)
        if (e.displayPath == sFilePath) { e.tags = tags; break; }

    m_bDirty = true;
    return true;
}

QStringList FileManager::allTags() const
{
    QSet<QString> seen;
    QStringList out;
    for (const FILEINFO &fi : m_vtFileInfos)
    {
        for (const QString &tag : fi.listTags)
        {
            const QString key = tag.toCaseFolded();
            if (seen.contains(key))
                continue;
            seen.insert(key);
            out << tag;
        }
    }
    std::sort(out.begin(), out.end(), [](const QString &a, const QString &b) {
        return QString::localeAwareCompare(a, b) < 0;
    });
    return out;
}

bool FileManager::save()
{
    if (!isArchiveLoaded())
    {
        setError(QStringLiteral("没有已加载的归档文件，无法保存"));
        return false;
    }
    if (isBusy())
    {
        setError(QStringLiteral("另有文件操作正在进行，请稍后重试"));
        return false;
    }
    if (!m_bDirty) return true;

    if (m_vtFileInfos.isEmpty())
    {
        // 不在保存路径里静默删除归档；用户仍可用“卸载文件”或手动删除。
        setError(QStringLiteral("归档内已无条目，保存会删除归档文件。"
                                "请使用“卸载文件”，或先添加文件后再保存。"));
        return false;   // m_bDirty 保持 true，改动仍在内存中
    }

    const bool bOk = rewriteArchive();
    if (bOk) m_bDirty = false;
    // 失败时保持 dirty：改动仍在内存中，用户可重试
    return bOk;
}

// ============================================================
// 归档写入：头部占位 → 流式复制内容 → 回填 CRC
// ============================================================

bool FileManager::writeArchiveHeader(QFile &fOut, const QVector<FILEINFO> &infos,
                                     const QString &sRoot,
                                     QVector<qint64> &outCrcFieldPos,
                                     QVector<qint64> &outContentPos,
                                     qint64 &outHeaderSize, QString &sErr,
                                     const QStringList *pStoredPaths)
{
    auto fail = [&sErr](const QString &m) { sErr = m; return false; };

    const QByteArray rootBytes = sRoot.toUtf8();
    if (rootBytes.size() > int(FwArc::MAX_ROOT_LEN))
        return fail(QStringLiteral("原始根过长"));

    QVector<QByteArray> rels;
    QVector<QStringList> tagsList;
    rels.reserve(infos.size());
    tagsList.reserve(infos.size());
    qint64 hs = FwArc::GLOBAL_HDR + 4 + rootBytes.size();
    for (int i = 0; i < infos.size(); ++i)
    {
        const FILEINFO &fi = infos.at(i);
        // 正常合并：按"绝对路径相对公共根"生成；展开已有归档时调用方直接给存储路径
        QString rel = (pStoredPaths != nullptr && pStoredPaths->size() == infos.size())
                          ? pStoredPaths->at(i)
                          : FwArc::makeRelative(fi.sFilePath, sRoot);
        rel = FwArc::sanitizeRelativePath(rel);
        const QByteArray rb = rel.toUtf8();
        if (rb.size() > int(FwArc::MAX_PATH_LEN))
            return fail(QStringLiteral("条目路径过长: %1").arg(fi.sFilePath));
        // 标记先归一化再计入头部大小，写头部时直接用这份结果，避免两处规则漂移。
        QString tagErr;
        const QStringList tags = FwArc::normalizeTags(fi.listTags, &tagErr);
        if (!tagErr.isEmpty())
            return fail(QStringLiteral("第 %1 条标记无效：%2").arg(i).arg(tagErr));
        rels.append(rb);
        tagsList.append(tags);
        hs += qint64(FwArc::ENTRY_FIXED) + qint64(rb.size())
              + qint64(FwArc::tagPayloadBytes(tags));
    }
    if (hs < FwArc::GLOBAL_HDR || quint64(hs) > FwArc::MAX_HDR_SIZE
        || quint64(hs) > 0xFFFFFFFFULL)
        return fail(QStringLiteral("归档头部过大: %1 B").arg(hs));

    if (!fOut.seek(0)) return fail(QStringLiteral("定位输出文件失败"));
    if (fOut.write(FwArc::MAGIC, 4) != 4) return fail(QStringLiteral("写入魔数失败"));
    bool ok = FwArc::writeU32(fOut, FwArc::CURRENT_VERSION)
           && FwArc::writeU32(fOut, quint32(hs))
           && FwArc::writeU32(fOut, quint32(infos.size()))
           && FwArc::writeU32(fOut, 0)      // HeaderChecksum 占位
           && FwArc::writeU32(fOut, 0)      // Reserved
           && FwArc::writeU32(fOut, quint32(rootBytes.size()));
    if (!ok) return fail(QStringLiteral("写入全局头失败: %1").arg(fOut.errorString()));
    if (!rootBytes.isEmpty() && fOut.write(rootBytes) != rootBytes.size())
        return fail(QStringLiteral("写入原始根失败: %1").arg(fOut.errorString()));

    qint64 cp = hs;
    outCrcFieldPos.clear();
    outContentPos.clear();
    for (int i = 0; i < infos.size(); ++i)
    {
        const FILEINFO &fi = infos.at(i);
        ok = FwArc::writeU32(fOut, quint32(rels.at(i).size()))
          && FwArc::writeU64(fOut, quint64(fi.nFileLen))
          && FwArc::writeU64(fOut, quint64(cp));
        if (!ok) return fail(QStringLiteral("写入第 %1 条记录失败").arg(i));
        outCrcFieldPos.append(fOut.pos());
        if (!FwArc::writeU32(fOut, 0))    // ContentCrc32 占位
            return fail(QStringLiteral("写入第 %1 条 CRC 字段失败").arg(i));
        // 时间统一收敛到可显示范围，属性只保留 R/H/S/A 位
        const quint64 created  = FwMeta::isPlausibleTimestamp(fi.nCreatedTime)
                                     ? quint64(fi.nCreatedTime) : 0;
        const quint64 modified = FwMeta::isPlausibleTimestamp(fi.nModifiedTime)
                                     ? quint64(fi.nModifiedTime) : 0;
        const quint64 accessed = FwMeta::isPlausibleTimestamp(fi.nAccessedTime)
                                     ? quint64(fi.nAccessedTime) : 0;
        ok = FwArc::writeU64(fOut, created)
          && FwArc::writeU64(fOut, modified)
          && FwArc::writeU64(fOut, accessed)
          && FwArc::writeU32(fOut, fi.nAttributes & FwMeta::ATTR_MASK);
        if (!ok) return fail(QStringLiteral("写入第 %1 条元数据失败").arg(i));
        // 固定字段末尾的 TagCount，然后是路径，最后是标记正文
        if (!FwArc::writeU32(fOut, quint32(tagsList.at(i).size())))
            return fail(QStringLiteral("写入第 %1 条标记数量失败").arg(i));
        if (!rels.at(i).isEmpty() && fOut.write(rels.at(i)) != rels.at(i).size())
            return fail(QStringLiteral("写入第 %1 条路径失败").arg(i));
        for (const QString &tag : tagsList.at(i))
        {
            const QByteArray tb = tag.toUtf8();
            if (tb.isEmpty() || tb.size() > int(FwArc::MAX_TAG_LEN))
                return fail(QStringLiteral("第 %1 条标记长度异常").arg(i));
            ok = FwArc::writeU32(fOut, quint32(tb.size()))
              && fOut.write(tb) == tb.size();
            if (!ok) return fail(QStringLiteral("写入第 %1 条标记失败").arg(i));
        }
        outContentPos.append(cp);
        cp += infos.at(i).nFileLen;
    }
    outHeaderSize = hs;
    return true;
}

bool FileManager::finalizeArchiveHeader(QFile &fOut, qint64 headerSize,
                                        const QVector<qint64> &crcFieldPos,
                                        const QVector<quint32> &contentCrcs,
                                        QString &sErr)
{
    auto fail = [&sErr](const QString &m) { sErr = m; return false; };
    if (crcFieldPos.size() != contentCrcs.size())
        return fail(QStringLiteral("内部错误：CRC 字段数量不匹配"));

    for (int i = 0; i < crcFieldPos.size(); ++i)
    {
        if (!fOut.seek(crcFieldPos.at(i))) return fail(QStringLiteral("定位第 %1 条 CRC 字段失败").arg(i));
        if (!FwArc::writeU32(fOut, contentCrcs.at(i)))
            return fail(QStringLiteral("回填第 %1 条 CRC 失败").arg(i));
    }

    // 头部 CRC 覆盖：全局头[0..15] + [20..23] + 头部区[24..headerSize)
    if (!fOut.seek(0)) return fail(QStringLiteral("定位头部失败"));
    char gh[FwArc::GLOBAL_HDR];
    if (fOut.read(gh, FwArc::GLOBAL_HDR) != FwArc::GLOBAL_HDR)
        return fail(QStringLiteral("回读全局头失败"));
    FwArc::Crc32 crc;
    crc.update(gh, 16);
    crc.update(gh + 20, 4);

    if (!fOut.seek(FwArc::GLOBAL_HDR)) return fail(QStringLiteral("定位头部区失败"));
    QByteArray buf;
    buf.resize(int(kCrcChunk));
    qint64 remain = headerSize - FwArc::GLOBAL_HDR;
    while (remain > 0)
    {
        const qint64 n = qMin(remain, kCrcChunk);
        const qint64 got = fOut.read(buf.data(), n);
        if (got != n) return fail(QStringLiteral("回读头部区不完整"));
        crc.update(buf.data(), got);
        remain -= got;
    }
    if (!fOut.seek(16)) return fail(QStringLiteral("定位校验和字段失败"));
    if (!FwArc::writeU32(fOut, crc.value()))
        return fail(QStringLiteral("回填头部校验和失败"));
    if (!fOut.flush()) return fail(QStringLiteral("刷新输出文件失败: %1").arg(fOut.errorString()));
    return true;
}

// 流式复制 [srcPos, srcPos+len)，同时计算内容 CRC、上报进度、响应取消。
// 注意：fSrc 必须是**本操作独占**的文件句柄（P1-1）。共享 m_qFile 的游标会在
// processEvents() 期间被 entryHead()/entryContent() 再次 seek，导致错位读取。
bool FileManager::streamCopy(QFile &fSrc, qint64 srcPos, qint64 len, QFile &fDst,
                             FwArc::Crc32 &crc, qint64 &copiedTotal, qint64 totalBytes,
                             const QString &sStatusPrefix, QString &sErr)
{
    auto fail = [&sErr](const QString &m) { sErr = m; return false; };
    if (!fSrc.seek(srcPos)) return fail(QStringLiteral("定位源内容失败: %1").arg(fSrc.errorString()));

    QByteArray buf;
    buf.resize(int(kCopyChunk));
    qint64 done = 0;
    int nLastPercent = -1;
    QElapsedTimer pumpTimer;
    pumpTimer.start();
    while (done < len)
    {
        if (m_bCancelRequested) return fail(QStringLiteral("操作已取消"));
        const qint64 n = qMin(len - done, kCopyChunk);
        const qint64 got = fSrc.read(buf.data(), n);
        if (got <= 0)
            return fail(QStringLiteral("读取源内容不完整 (%1): %2")
                        .arg(fSrc.fileName(), fSrc.errorString()));
        if (fDst.write(buf.data(), got) != got)
            return fail(QStringLiteral("写入目标失败 (%1): %2")
                        .arg(fDst.fileName(), fDst.errorString()));
        crc.update(buf.data(), got);
        done += got;
        copiedTotal += got;

        const int pct = (totalBytes > 0) ? int(100 * copiedTotal / totalBytes) : 100;
        if (pct != nLastPercent)
        {
            nLastPercent = pct;
            // 状态文字与进度条统一使用累计口径（P2-4）
            emit progressChanged(copiedTotal, totalBytes,
                QStringLiteral("%1  (%2 / %3)")
                    .arg(sStatusPrefix, FwArc::formatSize(copiedTotal), FwArc::formatSize(totalBytes)));
            // 限频泵事件：仍能让进度框刷新/响应取消，但不再每 1 MB 就给
            // 重入窗口一次机会（P1-1 的根因已由独占句柄消除，这里进一步收窄）。
            if (pumpTimer.elapsed() >= kProgressPumpIntervalMs)
            {
                pumpTimer.restart();
                QApplication::processEvents();
            }
        }
    }
    return true;
}

// ============================================================
// 合并（P0-1 / P1-7 / P2-4 / P3-2）
// ============================================================

void FileManager::QMergeFiles(QVector<QString> vtInputFiles, QString sOutputFile)
{
    if (isBusy()) { emit operationFinished(false, tr("另有文件操作正在进行")); return; }
    BusyGuard guard(this);

    auto finish = [this](bool ok, const QString &msg) {
        if (!ok) setError(msg);
        emit operationFinished(ok, msg);
    };

    if (vtInputFiles.isEmpty()) { finish(false, tr("没有要合并的文件")); return; }
    if (sOutputFile.isEmpty())  { finish(false, tr("输出路径为空")); return; }

    const QFileInfo outInfo(sOutputFile);
    if (outInfo.isDir())
    {
        finish(false, tr("输出路径是一个已存在的目录，请选择文件路径：%1").arg(sOutputFile));
        return;
    }
    if (isArchiveLoaded()
        && QFileInfo(m_qFile.fileName()).absoluteFilePath().compare(
               outInfo.absoluteFilePath(), Qt::CaseInsensitive) == 0)
    {
        finish(false, tr("输出不能是当前已加载的归档：%1\n请先卸载该归档，或选择其它输出文件。")
                           .arg(sOutputFile));
        return;
    }

    // —— P0-1：输出文件不能是任一输入文件（canonical 比较，兼容大小写与相对路径）——
    const QString outCanon = QFileInfo(sOutputFile).canonicalFilePath();
    const QString outAbs   = QFileInfo(sOutputFile).absoluteFilePath();
    for (const QString &in : vtInputFiles)
    {
        const QString inCanon = QFileInfo(in).canonicalFilePath();
        const QString inAbs   = QFileInfo(in).absoluteFilePath();
        const bool same = (!outCanon.isEmpty() && !inCanon.isEmpty()
                           && outCanon.compare(inCanon, Qt::CaseInsensitive) == 0)
                       || outAbs.compare(inAbs, Qt::CaseInsensitive) == 0;
        if (same)
        {
            finish(false, tr("输出文件不能是待合并文件之一：%1\n"
                             "这会先截断源文件再读取，导致数据丢失。").arg(in));
            return;
        }
    }

    // 输出目录必须存在
    const QString outDir = QFileInfo(sOutputFile).absolutePath();
    if (!QDir().exists(outDir))
    {
        finish(false, tr("输出目录不存在：%1").arg(outDir));
        return;
    }

    // P0-1：同一输出文件的跨进程写锁。没有它，两个实例会同时把对方正在写的
    // 辅助文件/备份当成自己的旧文件处理。
    OutputWriteLock outputLock;
    QString sLockErr;
    if (!outputLock.acquire(outInfo.absoluteFilePath(), sLockErr))
    {
        finish(false, sLockErr);
        return;
    }

    // ---- 收集输入：已合并的 FWDA 归档先拆分成条目，再一起合并 ----
    // 把归档当成普通文件嵌进输出会变成"归档套归档"，用户还要再拆一层；
    // 这里直接读它的索引，把每个条目作为独立输入展开。
    //
    // 关键点：每个条目都要记住**它原本的完整路径**（归档 = 该归档的 root + 归档内相对路径；
    // 普通文件 = 绝对路径），最后再统一决定外层归档的 root。否则会出现
    // "E:/testfiles_copy 里的文件合并进来后被挂到 D: 下"这种归属错乱。
    struct MergeSource {
        QString sSourcePath;      // 实际读取的文件（普通文件或 FWDA 归档）
        qint64  nOffset   = 0;    // 内容在源文件里的偏移
        qint64  nLength   = 0;
        QString sOriginPath;      // 原本的完整展示路径（决定它归到哪个目录）
        QString sStoredPath;      // 写入输出归档的路径（由 sOriginPath 推导）
        QString sDisplayName;     // 进度与错误提示
        bool    bFromArchive = false;
        // 源文件的文件系统元数据（普通文件现读；展开归档时沿用条目里存的）
        FwMeta::Metadata fsMeta;
        // 展开已有归档时沿用条目的标记；普通文件没有标记
        QStringList listTags;
        // P1-2：来源归档条目的内容 CRC。复制完必须与它比对，否则「结构完好但
        // 内容已损坏」的输入会在二次合并后被重新算出 CRC、变成看似完好的归档。
        quint32 nContentCrc = 0;
        // P1-6：收集时记录来源大小，复制前后各核对一次，防止静默保存旧长度前缀。
        qint64  nSourceSize = -1;
    };

    QVector<MergeSource> sources;
    QStringList allOrigins;       // 所有条目的原始完整路径
    int nArchiveInputs = 0;       // 被拆分的归档个数
    int nArchiveEntries = 0;      // 展开出来的条目数

    for (const QString &fp : vtInputFiles)
    {
        QFileInfo fi(fp);
        if (!fi.exists() || !fi.isFile())
        {
            finish(false, tr("输入文件不存在或不是普通文件：%1").arg(fp));
            return;
        }

        // 先读元数据再碰文件内容：下面探测 FWDA 魔数会读 4 个字节，
        // 而「读文件」本身会刷新访问时间（部分机器启用了 atime 更新）。
        // 普通文件用这份；展开归档时用条目里存的元数据。
        const FwMeta::Metadata sourceMeta = FwMeta::capture(fi.absoluteFilePath());

        // 已经是 FWDA 归档：读索引并展开条目
        bool bFwda = false;
        {
            QFile probe(fp);
            if (probe.open(QIODevice::ReadOnly))
            {
                char magic[4] = { 0, 0, 0, 0 };
                bFwda = probe.read(magic, 4) == 4
                        && std::memcmp(magic, FwArc::MAGIC, 4) == 0;
            }
        }
        if (bFwda)
        {
            FwArc::Meta meta;
            QString sArcErr;
            if (!inspectArchive(fp, meta, sArcErr))
            {
                finish(false, tr("无法读取已合并归档：%1\n%2").arg(fp, sArcErr));
                return;
            }
            for (const FwArc::Entry &e : meta.entries)
            {
                MergeSource s;
                s.sSourcePath  = fp;
                s.nOffset      = qint64(e.contentPos);
                s.nLength      = qint64(e.size);
                // 用来源归档的 root 还原它原本的完整路径；手工构造的归档可能没有 root。
                s.sOriginPath = e.displayPath.isEmpty()
                    ? (meta.root.isEmpty() ? e.relPath
                                           : meta.root + QLatin1Char('/') + e.relPath)
                    : e.displayPath;
                s.sDisplayName = QFileInfo(e.relPath).fileName();
                s.bFromArchive = true;
                s.nContentCrc = e.contentCrc;
                s.nSourceSize = qint64(meta.fileSize);
                s.fsMeta.valid      = true;
                s.fsMeta.createdMs  = qint64(e.createdMs);
                s.fsMeta.modifiedMs = qint64(e.modifiedMs);
                s.fsMeta.accessedMs = qint64(e.accessedMs);
                s.fsMeta.attributes = e.attributes;
                s.listTags = e.tags;   // 展开归档时保留标记
                sources.append(s);
                allOrigins << s.sOriginPath;
                ++nArchiveEntries;
            }
            ++nArchiveInputs;
            continue;
        }

        MergeSource s;
        s.sSourcePath  = fi.absoluteFilePath();
        s.nLength      = fi.size();
        s.sOriginPath  = fi.absoluteFilePath();
        s.sDisplayName = fi.fileName();
        s.nSourceSize  = fi.size();
        // 合并时把源文件的时间/属性一起收进归档
        s.fsMeta       = sourceMeta;
        sources.append(s);
        allOrigins << s.sOriginPath;
    }

    if (sources.isEmpty())
    {
        finish(false, tr("没有可合并的内容（选中的归档都是空的）"));
        return;
    }

    // ---- 决定外层归档的 root ----
    // 理想情况：所有条目都在同一个公共目录下（常见于普通合并），storePath 取相对路径，
    // 显示时 root + 相对路径，既短又能保留目录结构。
    // 跨盘符 / 无公共根时（如 E: 的归档 + D: 的文件）：root 只能为空，此时把盘符
    // 当成首段（"E:/x" → "E/x"）写进路径，保证来源目录不会丢。
    QString sRoot = FwArc::commonRoot(allOrigins);
    bool bFlatOrigin = sRoot.isEmpty();
    if (!bFlatOrigin)
    {
        for (const MergeSource &s : sources)
        {
            const QString o = FwArc::normalizePath(s.sOriginPath);
            if (!(o.compare(sRoot, Qt::CaseInsensitive) == 0
                  || o.startsWith(sRoot + QLatin1Char('/'), Qt::CaseInsensitive)))
            {
                bFlatOrigin = true;      // 公共根其实覆盖不到该条目，退回扁平模式
                break;
            }
        }
    }
    if (bFlatOrigin) sRoot.clear();

    for (MergeSource &s : sources)
    {
        if (!bFlatOrigin)
        {
            s.sStoredPath = FwArc::sanitizeRelativePath(
                FwArc::makeRelative(s.sOriginPath, sRoot));
        }
        else
        {
            QString o = FwArc::normalizePath(s.sOriginPath);
            if (o.size() >= 2 && o.at(1) == QLatin1Char(':') && o.at(0).isLetter())
                o = o.left(1) + o.mid(2);        // "E:/x" -> "E/x"（冒号不能进路径段）
            s.sStoredPath = FwArc::sanitizeRelativePath(o);
        }
    }

    // 路径去重：展开归档后很容易撞名（归档里已有 photo.png，同时又选中了 photo.png）。
    // 撞名时在扩展名前加 " (2)"、" (3)"…，与原有条目区分开。
    int nRenamedTotal = 0;
    {
        QSet<QString> used;
        // 每个基名记住上次用到的序号：第 k 个重名从 k 继续试，而不是每次都从 1 重来
        // （原来 N 个同名条目是 O(N²)，且循环里没有上限/取消检查）。
        QHash<QString, int> nextIndex;
        int nRenamed = 0;
        int nProbe = 0;
        for (MergeSource &s : sources)
        {
            if (m_bCancelRequested)
            {
                finish(false, tr("操作已取消"));
                return;
            }
            const QString base = s.sStoredPath;
            const int slash = base.lastIndexOf(QLatin1Char('/'));
            const QString dir  = slash >= 0 ? base.left(slash + 1) : QString();
            const QString name = slash >= 0 ? base.mid(slash + 1) : base;
            const int dot = name.lastIndexOf(QLatin1Char('.'));
            const QString stem = dot > 0 ? name.left(dot) : name;
            const QString ext  = dot > 0 ? name.mid(dot) : QString();
            const QString key = base.toLower();

            QString candidate = base;
            if (used.contains(key))
            {
                int n = nextIndex.value(key, 1);
                while (used.contains(candidate.toLower()))
                {
                    if (++n > 100000)   // 与 chooseOutputPath 同量级的硬上限，避免病态输入死循环
                    {
                        finish(false, tr("同名文件过多，无法自动改名：%1").arg(base));
                        return;
                    }
                    candidate = dir + stem + QStringLiteral(" (%1)").arg(n) + ext;
                    if ((++nProbe & 0x3FF) == 0)
                    {
                        QApplication::processEvents();
                        if (m_bCancelRequested) { finish(false, tr("操作已取消")); return; }
                    }
                }
                nextIndex.insert(key, n);
            }
            if (candidate != base) ++nRenamed;
            s.sStoredPath = candidate;
            used.insert(candidate.toLower());
        }
        nRenamedTotal = nRenamed;
        if (nRenamed > 0)
            qWarning() << "[Merge] 有" << nRenamed << "个条目路径重复，已自动改名";
    }

    // FILEINFO 仍作为写入结构（内容偏移由 sources 决定）
    QVector<FILEINFO> infos;
    QStringList storedPaths;
    qint64 totalBytes = 0;
    for (const MergeSource &s : sources)
    {
        FILEINFO info;
        info.sFilePath       = s.sStoredPath;
        info.sFileName       = s.sDisplayName;
        info.nFileLen        = s.nLength;
        info.nContentPositon = s.nOffset;
        info.nState          = 0;
        if (s.fsMeta.valid)
        {
            info.bHasFsMetadata = true;
            info.nCreatedTime   = s.fsMeta.createdMs;
            info.nModifiedTime  = s.fsMeta.modifiedMs;
            info.nAccessedTime  = s.fsMeta.accessedMs;
            info.nAttributes    = s.fsMeta.attributes;
        }
        info.listTags = s.listTags;
        infos.append(info);
        storedPaths << s.sStoredPath;
        totalBytes += s.nLength;
    }

    const qint64 rootBytes = qint64(sRoot.toUtf8().size());
    qint64 needBytes = FwArc::GLOBAL_HDR + 4 + rootBytes
                     + qint64(FwArc::ENTRY_FIXED) * qint64(infos.size()) + totalBytes;
    for (int i = 0; i < storedPaths.size(); ++i)
    {
        const qint64 one = qint64(storedPaths.at(i).toUtf8().size())
                         + qint64(FwArc::tagPayloadBytes(infos.at(i).listTags));
        if (!FwArc::checkedAdd64(needBytes, one, &needBytes))
        {
            finish(false, tr("输出规模超出可表示范围"));
            return;
        }
    }

    // 磁盘空间预检（P3-2）
    const qint64 freeBytes = FwArc::availableBytes(sOutputFile);
    if (freeBytes >= 0 && freeBytes < needBytes)
    {
        finish(false, tr("磁盘空间不足：需要 %1，可用 %2")
               .arg(FwArc::formatSize(needBytes), FwArc::formatSize(freeBytes)));
        return;
    }

    // P0-1：在输出目录里以「随机名 + NewOnly」原子创建临时文件，
    // 由本次操作独占持有；绝不再使用 <输出>.tmp.fwda 这种固定名，
    // 也不再预先 remove() 一个不是本次创建的路径。
    OwnedAuxFiles aux;
    QString sTmp;
    QString sAuxErr;
    if (!createUniqueTempFile(sOutputFile, sTmp, sAuxErr))
    {
        finish(false, tr("创建临时文件失败：%1").arg(sAuxErr));
        return;
    }
    aux.add(sTmp);

    // 防御性检查：任何输入都不能恰好是本次刚创建的辅助文件
    for (const MergeSource &s : sources)
    {
        if (sameFilePath(s.sSourcePath, sTmp))
        {
            finish(false, tr("输入文件与本次操作的临时文件冲突：%1").arg(s.sSourcePath));
            return;
        }
    }

    QFile fTmp(sTmp);
    // 需要回读头部区来重算 CRC，因此临时文件必须以读写方式打开
    if (!fTmp.open(QIODevice::ReadWrite | QIODevice::Truncate))
    {
        finish(false, tr("创建临时文件失败：%1 (%2)").arg(sTmp, fTmp.errorString()));
        return;
    }

    QVector<qint64> crcFieldPos, contentPos;
    qint64 headerSize = 0;
    QString err;
    if (!writeArchiveHeader(fTmp, infos, sRoot, crcFieldPos, contentPos, headerSize, err,
                            &storedPaths))
    {
        fTmp.close();
        finish(false, tr("写入归档头部失败：%1").arg(err));
        return;
    }

    QVector<quint32> contentCrcs;
    contentCrcs.reserve(infos.size());
    qint64 copiedTotal = 0;
    bool bOk = true;
    for (int i = 0; i < sources.size() && bOk; ++i)
    {
        const MergeSource &s = sources.at(i);
        QFile src(s.sSourcePath);
        if (!src.open(QIODevice::ReadOnly))
        {
            err = tr("打开输入文件失败：%1 (%2)").arg(s.sSourcePath, src.errorString());
            bOk = false; break;
        }
        // P1-6：来源在收集后被外部修改（变长/变短）就直接失败，
        // 不静默保存「旧长度前缀」或读半截。
        if (s.nSourceSize >= 0 && src.size() != s.nSourceSize)
        {
            err = tr("来源文件在合并过程中发生变化（收集时 %1 B，当前 %2 B）：%3")
                      .arg(s.nSourceSize).arg(src.size()).arg(s.sSourcePath);
            bOk = false; break;
        }
        FwArc::Crc32 eCrc;
        bOk = streamCopy(src, s.nOffset, s.nLength, fTmp, eCrc,
                         copiedTotal, totalBytes,
                         tr("正在合并: %1\n").arg(s.sOriginPath), err);
        if (!bOk) break;
        // 复制期间来源不能变化（以文件大小作为低成本检查；内容 CRC 另算）
        if (s.nSourceSize >= 0 && src.size() != s.nSourceSize)
        {
            err = tr("来源文件在合并过程中被修改：%1").arg(s.sSourcePath);
            bOk = false; break;
        }
        // P1-2：来源归档条目的内容 CRC 必须与来源记录一致。结构完好的头部
        // 不能证明内容区没坏，不比对就会把损坏内容重新盖章成新归档。
        // 普通文件没有"来源 CRC"，复制结果本身就是要写入的 CRC。
        if (s.bFromArchive && eCrc.value() != s.nContentCrc)
        {
            err = tr("来源归档内容校验失败（CRC32 不匹配）：%1\n"
                     "该条目在来源归档中已损坏，拒绝把它合并成新归档。")
                      .arg(s.sOriginPath);
            bOk = false; break;
        }
        contentCrcs.append(eCrc.value());
        src.close();
    }
    if (bOk) bOk = finalizeArchiveHeader(fTmp, headerSize, crcFieldPos, contentCrcs, err);
    fTmp.close();

    if (bOk)
    {
        FwArc::Meta meta;
        bOk = parseFile(sTmp, meta, err);   // 完整解析 + 所有条目合法性校验
        if (!bOk) err = tr("临时归档校验失败：%1").arg(err);
    }
    if (!bOk)
    {
        finish(false, m_bCancelRequested ? tr("操作已取消") : tr("文件合并失败：%1").arg(err));
        return;
    }

    // 事务替换（P0-1 / P1-3）：原文件先移到唯一备份，新文件换入后**重新解析成功**
    // 才算成功；备份在确认成功后才删除。回滚失败时把备份绝对路径告诉用户。
    QString sBak;
    if (QFileInfo::exists(sOutputFile))
    {
        QString bakErr;
        if (!moveToUniqueBackup(sOutputFile, sBak, bakErr))
        {
            finish(false, tr("备份已有输出文件失败：%1\n%2").arg(sOutputFile, bakErr));
            return;
        }
        aux.add(sBak);
    }

    if (!QFile::rename(sTmp, sOutputFile))
    {
        QString rollbackErr;
        if (!sBak.isEmpty())
        {
            if (restoreFromBackup(sOutputFile, sBak, rollbackErr))
            {
                aux.release(sBak);
                finish(false, tr("替换输出文件失败：%1（目标可能被占用、只读或权限不足）")
                           .arg(sOutputFile));
            }
            else
            {
                aux.release(sBak);   // 保留备份供手工恢复
                finish(false, tr("替换输出文件失败，且回滚失败：%1\n原文件备份保留在：%2\n%3")
                           .arg(sOutputFile, sBak, rollbackErr));
            }
            return;
        }
        finish(false, tr("替换输出文件失败：%1（目标可能被占用、只读或权限不足）")
                   .arg(sOutputFile));
        return;
    }
    aux.release(sTmp);

    // 换入后必须能按新路径完整解析，否则回滚
    {
        FwArc::Meta chk;
        QString chkErr;
        if (!parseFile(sOutputFile, chk, chkErr))
        {
            QString rollbackErr;
            const bool restored = !sBak.isEmpty()
                                  && restoreFromBackup(sOutputFile, sBak, rollbackErr);
            if (!sBak.isEmpty()) aux.release(sBak);
            finish(false, restored
                ? tr("输出归档校验失败，已回滚到原文件：%1").arg(chkErr)
                : tr("输出归档校验失败：%1\n原文件备份保留在：%2").arg(chkErr, sBak));
            return;
        }
    }
    if (!sBak.isEmpty())
        aux.removeNow(sBak);

    QString sDone = tr("文件合并成功：共 %1 个文件，%2")
                        .arg(sources.size()).arg(FwArc::formatSize(totalBytes));
    // 输出路径必须回显成绝对路径：相对路径（用户填 "a"）是按进程当前目录解析的，
    // 从 IDE 启动时那就是 build/.../debug。提示里不写这一行，用户只能靠猜
    // 才知道 14 GB 的结果落在哪（2026-11-19 就发生过一次）。
    sDone += tr("\n输出文件：%1").arg(QFileInfo(sOutputFile).absoluteFilePath());
    if (nArchiveEntries > 0)
        sDone += tr("\n其中 %1 个条目来自 %2 个已合并归档（已先拆分再合并，不是嵌套归档）。")
                     .arg(nArchiveEntries).arg(nArchiveInputs);
    if (nRenamedTotal > 0)
        sDone += tr("\n另有 %1 个条目路径重复，已自动加 \" (2)\" 等后缀改名。")
                     .arg(nRenamedTotal);
    finish(true, sDone);
}

// ============================================================
// 保存：完整重写为当前格式（内容 CRC + 文件系统元数据 + 标记）
// ============================================================

// 写归档时的路径规划：与合并（QMergeFiles）用同一套规则，避免"保存后目录结构被拍平"。
// 之前 rewriteArchive 直接用 commonRoot + makeRelative：跨盘时 commonRoot 为空，
// makeRelative 退化成文件名，导致所有条目路径丢失、同名条目变成重复路径。
bool FileManager::computeStoredPaths(const QVector<FILEINFO> &infos,
                                     QString &sRoot, QStringList &storedPaths,
                                     QString &sErr)
{
    sRoot.clear();
    storedPaths.clear();

    if (infos.isEmpty())
    {
        sErr = tr("没有可写入的条目");
        return false;
    }

    QStringList allPaths;
    allPaths.reserve(infos.size());
    for (const FILEINFO &fi : infos)
        allPaths << fi.sFilePath;

    QString root = FwArc::commonRoot(allPaths);
    bool bFlat = root.isEmpty();
    if (!bFlat)
    {
        for (const FILEINFO &fi : infos)
        {
            const QString p = FwArc::normalizePath(fi.sFilePath);
            if (!(p.compare(root, Qt::CaseInsensitive) == 0
                  || p.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive)))
            {
                bFlat = true;     // 公共根覆盖不到该条目，退回扁平模式
                break;
            }
        }
    }
    if (bFlat) root.clear();

    QHash<QString, QString> seen;   // 小写路径 -> 原始路径
    for (const FILEINFO &fi : infos)
    {
        QString rel;
        if (!bFlat)
        {
            rel = FwArc::makeRelative(fi.sFilePath, root);
        }
        else
        {
            QString p = FwArc::normalizePath(fi.sFilePath);
            if (p.size() >= 2 && p.at(1) == QLatin1Char(':') && p.at(0).isLetter())
                p = p.left(1) + p.mid(2);          // "E:/x" -> "E/x"
            rel = p;
        }
        rel = FwArc::sanitizeRelativePath(rel);
        if (rel.isEmpty())
        {
            sErr = tr("无法为条目生成归档内路径：%1").arg(fi.sFilePath);
            return false;
        }

        const QString key = rel.toLower();
        if (seen.contains(key))
        {
            sErr = tr("归档内出现重复路径，保存后无法区分条目：%1\n"
                      "请先删除同名条目（或调整其所在目录）后重试。")
                       .arg(rel);
            return false;
        }
        seen.insert(key, rel);
        storedPaths.append(rel);
    }

    sRoot = root;
    return true;
}

bool FileManager::rewriteArchive()
{
    if (!isArchiveLoaded()) { setError(tr("没有已加载的归档")); return false; }
    BusyGuard guard(this);

    const QString sArchivePath = m_qFile.fileName();
    QVector<FILEINFO> infos = m_vtFileInfos;

    // 新增但尚未保存的条目在保存时重新读一次源文件元数据，
    // 让归档里记录的时间和属性与真正写进去的内容一致（源文件可能在添加后被修改过）。
    for (FILEINFO &fi : infos)
    {
        if (!m_setPendingFiles.contains(fi.sFilePath))
            continue;
        const FwMeta::Metadata m = FwMeta::capture(fi.sFilePath);
        if (m.valid)
        {
            fi.bHasFsMetadata = true;
            fi.nCreatedTime   = m.createdMs;
            fi.nModifiedTime  = m.modifiedMs;
            fi.nAccessedTime  = m.accessedMs;
            fi.nAttributes    = m.attributes;
        }
    }

    // 删除与新增统一走下面的完整事务重写（临时文件 + 校验 + 替换 + 重新解析）。

    // P1-6：归档在加载后被外部改写（大小变化）时，内存里的条目偏移已不可信。
    // 继续保存会把「按旧偏移读出来的字节」当成新归档内容，直接中止。
    const qint64 nArchiveSizeNow = QFileInfo(sArchivePath).size();
    if (nArchiveSizeNow < 0 || quint64(nArchiveSizeNow) != m_meta.fileSize)
    {
        setError(tr("归档文件在加载后被外部修改（加载时 %1 B，当前 %2 B），"
                    "为避免写入错误内容已中止保存。请重新加载归档后重试。")
                 .arg(m_meta.fileSize).arg(nArchiveSizeNow));
        return false;
    }

    // P0-1：同一归档的跨进程写锁
    OutputWriteLock outputLock;
    QString sLockErr;
    if (!outputLock.acquire(QFileInfo(sArchivePath).absoluteFilePath(), sLockErr))
    {
        setError(sLockErr);
        return false;
    }

    QString sRoot;
    QStringList storedPaths;
    QString pathErr;
    if (!computeStoredPaths(infos, sRoot, storedPaths, pathErr))
    {
        setError(pathErr);
        return false;
    }

    qint64 totalBytes = 0;
    for (const FILEINFO &fi : infos)
    {
        if (fi.nFileLen < 0
            || !FwArc::checkedAdd64(totalBytes, fi.nFileLen, &totalBytes))
        {
            setError(tr("条目总大小超出可表示范围，无法保存"));
            return false;
        }
    }

    // 头部按实际存储路径精确计算（含每条路径与标记字节），避免空间预检偏小
    qint64 headerBytes = 0;
    if (!FwArc::checkedAdd64(FwArc::GLOBAL_HDR + 4,
                             qint64(sRoot.toUtf8().size()), &headerBytes))
    {
        setError(tr("归档头部过大"));
        return false;
    }
    for (int i = 0; i < storedPaths.size(); ++i)
    {
        qint64 one = qint64(FwArc::ENTRY_FIXED)
                   + qint64(storedPaths.at(i).toUtf8().size());
        if (!FwArc::checkedAdd64(one,
                                 qint64(FwArc::tagPayloadBytes(infos.at(i).listTags)), &one)
            || !FwArc::checkedAdd64(headerBytes, one, &headerBytes))
        {
            setError(tr("归档头部过大"));
            return false;
        }
    }
    qint64 needBytes = 0;
    if (!FwArc::checkedAdd64(headerBytes, totalBytes, &needBytes))
    {
        setError(tr("保存规模超出可表示范围"));
        return false;
    }
    const qint64 freeBytes = FwArc::availableBytes(sArchivePath);
    if (freeBytes >= 0 && freeBytes < needBytes)
    {
        setError(tr("磁盘空间不足，无法保存：需要 %1，可用 %2")
                 .arg(FwArc::formatSize(needBytes), FwArc::formatSize(freeBytes)));
        return false;
    }

    // P0-1：唯一临时文件，由本次操作独占；不再使用 <归档>.tmp.fwda
    OwnedAuxFiles aux;
    QString sTmp;
    QString sAuxErr;
    if (!createUniqueTempFile(sArchivePath, sTmp, sAuxErr))
    {
        setError(tr("创建临时归档失败：%1").arg(sAuxErr));
        return false;
    }
    aux.add(sTmp);

    QFile fTmp(sTmp);
    // 需要回读头部区来重算 CRC，因此临时文件必须以读写方式打开
    if (!fTmp.open(QIODevice::ReadWrite | QIODevice::Truncate))
    {
        setError(tr("创建临时归档失败：%1 (%2)").arg(sTmp, fTmp.errorString()));
        return false;
    }

    QVector<qint64> crcFieldPos, contentPos;
    qint64 headerSize = 0;
    QString err;
    if (!writeArchiveHeader(fTmp, infos, sRoot, crcFieldPos, contentPos, headerSize, err,
                            &storedPaths))
    {
        fTmp.close();
        setError(tr("写入归档头部失败：%1").arg(err));
        return false;
    }

    // P1-1：整次保存使用一个本操作独占的源归档句柄（不共享 m_qFile）。
    // 这样即使进度泵/延迟渲染在本函数期间重入 entryHead()，也不会把复制游标改走。
    QFile srcArc(sArchivePath);
    if (!srcArc.open(QIODevice::ReadOnly))
    {
        fTmp.close();
        setError(tr("打开归档失败：%1 (%2)").arg(sArchivePath, srcArc.errorString()));
        return false;
    }

    QVector<quint32> contentCrcs;
    contentCrcs.reserve(infos.size());
    qint64 copiedTotal = 0;
    bool bOk = true;
    for (int i = 0; i < infos.size() && bOk; ++i)
    {
        const FILEINFO &fi = infos.at(i);
        FwArc::Crc32 eCrc;
        QFile pendingSrc;
        qint64 srcPos = 0;
        QFile *pSrc = &srcArc;
        if (m_setPendingFiles.contains(fi.sFilePath))
        {
            QString srcErr;
            if (!openEntrySource(fi.sFilePath, fi, pendingSrc, srcPos, srcErr))
            {
                err = srcErr;
                bOk = false; break;
            }
            pSrc = &pendingSrc;
        }
        else
        {
            srcPos = fi.nContentPositon;
        }

        bOk = streamCopy(*pSrc, srcPos, fi.nFileLen, fTmp, eCrc, copiedTotal, totalBytes,
                         tr("正在保存: %1").arg(fi.sFileName), err);
        // 复制期间新增文件被外部改写：不能把「读到的半截内容」当成完整条目
        if (bOk && pSrc == &pendingSrc && pendingSrc.size() != fi.nFileLen)
        {
            err = tr("文件在保存过程中被修改：%1").arg(fi.sFilePath);
            bOk = false;
        }
        if (!bOk) break;

        // 保存时不能把已损坏的内容重新盖章；与加载时记录的 CRC 比对。
        if (!m_setPendingFiles.contains(fi.sFilePath)
            && m_mapContentCrc.contains(fi.sFilePath)
            && eCrc.value() != m_mapContentCrc.value(fi.sFilePath))
        {
            err = tr("内容 CRC 与加载时不一致，归档可能已损坏：%1").arg(fi.sFilePath);
            bOk = false;
            break;
        }
        contentCrcs.append(eCrc.value());
    }
    srcArc.close();
    if (bOk) bOk = finalizeArchiveHeader(fTmp, headerSize, crcFieldPos, contentCrcs, err);
    fTmp.close();

    if (bOk)
    {
        FwArc::Meta meta;
        bOk = parseFile(sTmp, meta, err);
        if (!bOk) err = tr("临时归档校验失败：%1").arg(err);
    }
    if (!bOk)
    {
        setError(m_bCancelRequested ? tr("保存已取消")
                                    : tr("保存失败：%1\n改动仍保留在内存中，可重试。").arg(err));
        return false;
    }

    // Windows：必须先关闭自身句柄，否则 rename 会因共享冲突失败
    m_qFile.close();

    // P1-3：备份 -> 换入 -> 重新解析成功后才删除备份
    QString sBak;
    {
        QString bakErr;
        if (!moveToUniqueBackup(sArchivePath, sBak, bakErr))
        {
            // 备份失败：恢复自身只读句柄，保持内存状态与 dirty
            m_qFile.setFileName(sArchivePath);
            m_qFile.open(QIODevice::ReadOnly);
            setError(tr("备份原归档失败：%1\n%2\n改动仍保留在内存中，可重试。")
                     .arg(sArchivePath, bakErr));
            return false;
        }
        aux.add(sBak);
    }

    if (!QFile::rename(sTmp, sArchivePath))
    {
        QString rollbackErr;
        if (restoreFromBackup(sArchivePath, sBak, rollbackErr))
        {
            aux.release(sBak);
            m_qFile.setFileName(sArchivePath);
            m_qFile.open(QIODevice::ReadOnly);
            setError(tr("替换原归档失败：%1\n改动仍保留在内存中，可重试。").arg(sArchivePath));
        }
        else
        {
            aux.release(sBak);   // 保留备份供手工恢复
            setError(tr("替换原归档失败且回滚失败：%1\n原归档备份保留在：%2\n%3")
                     .arg(sArchivePath, sBak, rollbackErr));
        }
        return false;
    }
    aux.release(sTmp);

    // P1-3：重新加载成功才算保存成功（以前忽略 QLoadMergedFile 的返回值）
    if (!QLoadMergedFile(sArchivePath))
    {
        const QString sLoadErr = lastError();
        QString rollbackErr;
        if (!sBak.isEmpty())
        {
            if (m_qFile.isOpen()) m_qFile.close();
            if (restoreFromBackup(sArchivePath, sBak, rollbackErr))
            {
                aux.release(sBak);
                QLoadMergedFile(sArchivePath);   // 恢复到原内容，保持可浏览
                setError(tr("保存后的归档无法重新解析，已回滚到原文件：%1").arg(sLoadErr));
            }
            else
            {
                aux.release(sBak);
                setError(tr("保存后的归档无法重新解析：%1\n原归档备份保留在：%2\n%3")
                         .arg(sLoadErr, sBak, rollbackErr));
            }
        }
        else
        {
            setError(tr("保存后的归档无法重新解析：%1").arg(sLoadErr));
        }
        return false;
    }

    if (!sBak.isEmpty())
        aux.removeNow(sBak);
    return true;
}

void FileManager::cancelOperation()
{
    m_bCancelRequested = true;
}

// ============================================================
// 安全输出路径（P0-2 / P2-1 / P2-8）
// ============================================================

QString FileManager::chooseOutputPath(const QString &sBaseDir, const FwArc::Entry &e,
                                      bool bUseOldPath, QSet<QString> &setUsed,
                                      int &nRenamed)
{
    QString target;
    if (bUseOldPath)
    {
        QString why;
        target = absoluteOriginalPath(e.displayPath, &why);
        if (target.isEmpty())
            return QString();   // 拒绝回溯/非绝对路径，而不是落到 CWD
        if (FwArc::isReservedDeviceName(QFileInfo(target).fileName()))
            return QString();
        for (int i = 0; i < target.size(); ++i)
            if (target.at(i).unicode() < 32) return QString();
    }
    else
    {
        QString rel = e.relPath;
        QString why;
        if (!FwArc::isSafeRelativePath(rel, &why))
            rel = FwArc::sanitizeRelativePath(rel);
        target = FwArc::normalizePath(sBaseDir) + "/" + rel;
    }

    const QFileInfo t(target);
    const QString dir = t.absolutePath();
    const QString base = t.completeBaseName();
    const QString ext  = t.suffix();

    auto key = [](const QString &p) { return FwArc::normalizePath(QFileInfo(p).absoluteFilePath()).toLower(); };

    QString candidate = target;
    int n = 1;
    // 已存在的文件绝不覆盖；同一轮里名字冲突也自动加序号
    while (QFileInfo::exists(candidate) || setUsed.contains(key(candidate)))
    {
        candidate = dir + "/" + base + " (" + QString::number(n) + ")"
                  + (ext.isEmpty() ? QString() : "." + ext);
        ++n;
        if (n > 100000) return QString();
    }
    if (candidate != target) ++nRenamed;
    setUsed.insert(key(candidate));
    return candidate;
}

// ============================================================
// 分割（P0-2 / P1-4 / P1-7 / P2-1 / P2-3 / P2-5 / P3-2）
// ============================================================

void FileManager::QSplitFiles(QString sInputFile, bool bIsSaveAsOldPath, QString sSplitFileDir)
{
    if (isBusy()) { emit operationFinished(false, tr("另有文件操作正在进行")); return; }
    BusyGuard guard(this);

    auto finish = [this](bool ok, const QString &msg) {
        if (!ok) setError(msg);
        emit operationFinished(ok, msg);
    };

    FwArc::Meta meta;
    QString err;
    if (!parseFile(sInputFile, meta, err))
    {
        finish(false, tr("归档无效，已取消分割：%1").arg(err));
        return;
    }
    if (!bIsSaveAsOldPath)
    {
        if (sSplitFileDir.isEmpty()) { finish(false, tr("输出目录为空")); return; }
        if (!QDir().exists(sSplitFileDir) && !QDir().mkpath(sSplitFileDir))
        {
            finish(false, tr("创建输出目录失败：%1").arg(sSplitFileDir));
            return;
        }
    }

    QFile fIn(sInputFile);
    if (!fIn.open(QIODevice::ReadOnly))
    {
        finish(false, tr("打开归档失败：%1 (%2)").arg(sInputFile, fIn.errorString()));
        return;
    }

    qint64 totalBytes = 0;
    for (const FwArc::Entry &e : meta.entries)
    {
        if (!FwArc::checkedAdd64(totalBytes, qint64(e.size), &totalBytes))
        {
            finish(false, tr("归档条目总大小超出可表示范围"));
            return;
        }
    }

    // 目标盘空间预检（按第一个输出目录估算；跨盘条目无法精确，尽力而为）。
    // 只有固定输出目录才能准确预检；按原路径时条目分散在多盘，不能用一个卷判断。
    if (!bIsSaveAsOldPath)
    {
        const QStorageInfo storage(sSplitFileDir);
        if (storage.isValid() && storage.bytesAvailable() >= 0
            && totalBytes > storage.bytesAvailable())
        {
            finish(false, tr("目标磁盘空间不足：需要约 %1，可用 %2（%3）")
                               .arg(FwArc::formatSize(totalBytes),
                                    FwArc::formatSize(storage.bytesAvailable()), sSplitFileDir));
            return;
        }
    }

    qint64 copiedTotal = 0;
    QSet<QString> setUsed;
    int nRenamed = 0, nSkipped = 0, nFailed = 0, nDone = 0, nCrcBad = 0, nMetaWarn = 0;
    QString sFirstError;

    QByteArray buf;
    buf.resize(int(kCopyChunk));

    for (int i = 0; i < meta.entries.size(); ++i)
    {
        if (m_bCancelRequested) break;
        const FwArc::Entry &e = meta.entries.at(i);
        const QString sOut = chooseOutputPath(sSplitFileDir, e, bIsSaveAsOldPath, setUsed, nRenamed);
        if (sOut.isEmpty()) { ++nSkipped; continue; }

        const QString sDir = QFileInfo(sOut).absolutePath();
        if (!QDir().exists(sDir) && !QDir().mkpath(sDir))
        {
            if (sFirstError.isEmpty()) sFirstError = tr("创建输出目录失败：%1").arg(sDir);
            ++nFailed; continue;
        }

        // P1-5：用 NewOnly 原子创建，绝不「先 exists 再 Truncate」。
        // chooseOutputPath 只是选名，真正的“不覆盖”由内核的 create-new 保证。
        QFile fOut;
        bool bOpenedOut = false;
        QString sOutTry = sOut;
        for (int attempt = 0; attempt < 32 && !bOpenedOut; ++attempt)
        {
            fOut.setFileName(sOutTry);
            if (fOut.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            {
                bOpenedOut = true;
                break;
            }
            // 目标在选名之后被其它进程/条目创建：换一个带序号的唯一名重试
            if (!QFileInfo::exists(sOutTry))
                break;   // 不是“已存在”，而是权限/路径错误，交给失败分支
            const QFileInfo t(sOutTry);
            const QString sSuffix = t.suffix().isEmpty()
                                        ? QString() : QStringLiteral(".") + t.suffix();
            sOutTry = t.absolutePath() + QStringLiteral("/") + t.completeBaseName()
                      + QStringLiteral(" (") + QString::number(attempt + 2) + QStringLiteral(")")
                      + sSuffix;
            ++nRenamed;
        }
        if (!bOpenedOut)
        {
            if (sFirstError.isEmpty())
                sFirstError = tr("打开输出文件失败：%1（%2）").arg(sOutTry, fOut.errorString());
            ++nFailed;
            continue;
        }
        const QString sOutFinal = sOutTry;

        if (!fIn.seek(qint64(e.contentPos)))
        {
            fOut.close(); QFile::remove(sOutFinal);
            if (sFirstError.isEmpty()) sFirstError = tr("定位归档内容失败：%1").arg(e.relPath);
            ++nFailed; continue;
        }

        FwArc::Crc32 crc;
        qint64 done = 0;
        bool bOk = true;
        int nLastPercent = -1;
        QElapsedTimer splitPump;
        splitPump.start();
        while (done < qint64(e.size))
        {
            if (m_bCancelRequested) { bOk = false; break; }
            const qint64 n = qMin<qint64>(qint64(e.size) - done, kCopyChunk);
            const qint64 got = fIn.read(buf.data(), n);
            if (got <= 0) { bOk = false; break; }
            if (fOut.write(buf.data(), got) != got) { bOk = false; break; }
            crc.update(buf.data(), got);
            done += got;
            copiedTotal += got;

            const int pct = (totalBytes > 0) ? int(100 * copiedTotal / totalBytes) : 100;
            if (pct != nLastPercent)
            {
                nLastPercent = pct;
                // 与合并一致：第一行完整输出路径，第二行大小/剩余时间。
                // 路径过长由进度框只省略第一行路径，第二行保持完整。
                emit progressChanged(copiedTotal, totalBytes,
                    tr("正在分割: %1\n  (%2 / %3)")
                       .arg(QFileInfo(sOutFinal).absoluteFilePath(), FwArc::formatSize(copiedTotal),
                            FwArc::formatSize(totalBytes)));
                if (splitPump.elapsed() >= kProgressPumpIntervalMs)
                {
                    splitPump.restart();
                    QApplication::processEvents();
                }
            }
        }
        fOut.close();

        if (!bOk || done != qint64(e.size))
        {
            QFile::remove(sOutFinal);
            if (!m_bCancelRequested)
            {
                if (sFirstError.isEmpty()) sFirstError = tr("写入/读取失败：%1").arg(sOutFinal);
                ++nFailed;
            }
            if (m_bCancelRequested) break;
            continue;
        }
        // 内容 CRC 校验
        if (crc.value() != e.contentCrc)
        {
            QFile::remove(sOutFinal);
            ++nCrcBad;
            emit warningOccurred(tr("第 %1 个文件内容校验失败（CRC32 不匹配），已删除输出：%2")
                                .arg(i + 1).arg(sOutFinal));
            continue;
        }
        // 把条目录下的文件时间/属性写回磁盘。失败只告警，不删已写出的内容。
        QString sMetaErr;
        if (!FwMeta::apply(sOutFinal, qint64(e.createdMs), qint64(e.modifiedMs),
                           qint64(e.accessedMs), e.attributes, &sMetaErr))
        {
            ++nMetaWarn;
            emit warningOccurred(tr("文件已写出，但时间/属性恢复失败：%1\n%2")
                                     .arg(sOutFinal, sMetaErr));
        }
        ++nDone;
    }
    fIn.close();

    if (m_bCancelRequested)
    {
        finish(false, tr("分割已取消：已完成 %1 个，失败 %2 个").arg(nDone).arg(nFailed));
        return;
    }

    QString msg = tr("分割完成：成功 %1 个，跳过 %2 个，失败 %3 个，自动重命名 %4 个")
                    .arg(nDone).arg(nSkipped).arg(nFailed).arg(nRenamed);
    if (nCrcBad > 0) msg += tr("，内容校验失败 %1 个").arg(nCrcBad);
    if (nMetaWarn > 0) msg += tr("，时间/属性恢复失败 %1 个").arg(nMetaWarn);
    if (!sFirstError.isEmpty()) msg += tr("；首个错误：%1").arg(sFirstError);
    // P1-4：完全成功必须同时满足「无跳过、无失败、无 CRC 失败，且计数覆盖全部条目」。
    // 以前只看 failed/CRC，全部条目都被跳过时仍会发 operationFinished(true)。
    const int nTotal = meta.entries.size();
    const int nAccounted = nDone + nSkipped + nFailed + nCrcBad;
    const bool bAllDone = (nSkipped == 0 && nFailed == 0 && nCrcBad == 0
                           && nDone == nTotal && nAccounted == nTotal);
    if (!bAllDone)
        msg += tr("\n本次为部分完成：共 %1 个条目，成功 %2、跳过 %3、失败 %4、校验失败 %5。")
                   .arg(nTotal).arg(nDone).arg(nSkipped).arg(nFailed).arg(nCrcBad);
    finish(bAllDone, msg);
}

// ============================================================
// 读取与导出（P1-2 / P1-7 / P3-2）
// ============================================================

bool FileManager::contentCrcFor(const QString &sFilePath, quint32 *pCrc) const
{
    if (pCrc == nullptr || !m_mapContentCrc.contains(sFilePath)) return false;
    *pCrc = m_mapContentCrc.value(sFilePath);
    return true;
}

// 只读条目开头若干字节：格式嗅探只需要文件头，不必把整块内容读进内存。
QByteArray FileManager::entryHead(const QString &sFilePath, qint64 nMaxBytes, QString *psErr)
{
    if (psErr) psErr->clear();
    if (nMaxBytes <= 0) return QByteArray();

    auto it = m_mapFileInfos.find(sFilePath);
    if (it == m_mapFileInfos.end())
    {
        if (psErr) *psErr = tr("归档内未找到文件：%1").arg(sFilePath);
        return QByteArray();
    }

    const FILEINFO fi = it.value();
    if (fi.nFileLen <= 0) return QByteArray();

    QFile localSrc;
    qint64 srcPos = 0;
    QString sOpenErr;
    if (!openEntrySource(sFilePath, fi, localSrc, srcPos, sOpenErr))
    {
        if (psErr) *psErr = sOpenErr;
        return QByteArray();
    }

    const qint64 nWant = qMin<qint64>(fi.nFileLen, nMaxBytes);
    if (!FwArc::fitsInQsizetype(quint64(nWant)))
    {
        if (psErr) *psErr = tr("读取长度超出可处理范围：%1").arg(sFilePath);
        return QByteArray();
    }
    if (!localSrc.seek(srcPos))
    {
        if (psErr) *psErr = tr("定位文件内容失败：%1 (%2)").arg(sFilePath, localSrc.errorString());
        return QByteArray();
    }

    QByteArray out;
    out.resize(qsizetype(nWant));
    qint64 done = 0;
    while (done < nWant)
    {
        const qint64 n = qMin<qint64>(nWant - done, MAXBUFFERSIZE);
        const qint64 got = localSrc.read(out.data() + done, n);
        if (got <= 0) break;
        done += got;
    }
    out.resize(qsizetype(done));
    return out;
}

// C2：直接读出条目内容到 QByteArray，省掉"new char[] -> 插件再复制一份"的中转。
// 校验语义：大小、完整性与内容 CRC；读取失败时 psErr 给原因。
QByteArray FileManager::entryContent(const QString &sFilePath, QString *psErr)
{
    if (psErr) psErr->clear();

    auto it = m_mapFileInfos.find(sFilePath);
    if (it == m_mapFileInfos.end())
    {
        if (psErr) *psErr = tr("归档内未找到文件：%1").arg(sFilePath);
        return QByteArray();
    }

    const FILEINFO fi = it.value();
    if (fi.nFileLen < 0)
    {
        if (psErr) *psErr = tr("文件大小异常：%1").arg(sFilePath);
        return QByteArray();
    }
    if (fi.nFileLen == 0)
        return QByteArray();                        // 0 字节是合法情况
    if (!FwArc::fitsInQsizetype(quint64(fi.nFileLen)))
    {
        if (psErr) *psErr = tr("文件过大，超出当前平台可一次性读取的范围：%1").arg(sFilePath);
        return QByteArray();
    }

    QFile localSrc;
    qint64 srcPos = 0;
    QString sOpenErr;
    if (!openEntrySource(sFilePath, fi, localSrc, srcPos, sOpenErr))
    {
        if (psErr) *psErr = sOpenErr;
        return QByteArray();
    }

    QByteArray out;
    try
    {
        out.resize(qsizetype(fi.nFileLen));
    }
    catch (const std::bad_alloc &)
    {
        if (psErr) *psErr = tr("内存不足，无法一次性读取 %1（%2）。请改用导出或分割功能。")
                                .arg(fi.sFileName, FwArc::formatSize(fi.nFileLen));
        return QByteArray();
    }

    if (!localSrc.seek(srcPos))
    {
        if (psErr) *psErr = tr("定位文件内容失败：%1 (%2)").arg(sFilePath, localSrc.errorString());
        return QByteArray();
    }

    qint64 done = 0;
    while (done < fi.nFileLen)
    {
        const qint64 n = qMin<qint64>(fi.nFileLen - done, MAXBUFFERSIZE);
        const qint64 got = localSrc.read(out.data() + done, n);
        if (got <= 0)
        {
            if (psErr) *psErr = tr("读取文件内容不完整：%1（期望 %2 B，实际 %3 B）")
                                    .arg(sFilePath).arg(fi.nFileLen).arg(done);
            return QByteArray();
        }
        done += got;
    }

    if (m_mapContentCrc.contains(sFilePath))
    {
        FwArc::Crc32 c;
        c.update(out.constData(), fi.nFileLen);
        if (c.value() != m_mapContentCrc.value(sFilePath))
        {
            if (psErr) *psErr = tr("内容校验失败（CRC32 不匹配）：%1\n该文件可能已损坏。").arg(sFilePath);
            return QByteArray();
        }
    }

    if (psErr) psErr->clear();
    return out;
}

void FileManager::outputFile(QString sFilePath, QString sOutputDir)
{
    m_sLastError.clear();
    m_sLastOutputPath.clear();
    // 新操作开始：清掉上一次取消留下的标志，否则这里会"立刻取消"
    m_bCancelRequested = false;
    auto it = m_mapFileInfos.find(sFilePath);
    if (it == m_mapFileInfos.end())
    {
        setError(tr("归档内未找到文件：%1").arg(sFilePath));
        return;
    }
    const FILEINFO fi = it.value();

    QString sOut;
    if (!sOutputDir.isEmpty())
    {
        if (!QDir().exists(sOutputDir) && !QDir().mkpath(sOutputDir))
        {
            setError(tr("创建输出目录失败：%1").arg(sOutputDir)); return;
        }
        sOut = FwArc::normalizePath(sOutputDir) + "/" + fi.sFileName;
    }
    else
    {
        QString sPathErr;
        sOut = absoluteOriginalPath(sFilePath, &sPathErr);
        if (sOut.isEmpty())
        {
            setError(tr("无法按原路径导出（%1）：%2").arg(sPathErr, sFilePath));
            return;
        }
    }

    // P1-5：绝不静默覆盖已有文件，也不「先 exists 再 Truncate」。
    // 选名只是启发式，真正的互斥由 open(NewOnly) 内核创建保证：
    // 目标在检查与打开之间被别的进程创建时会换名重试；达到上限必须失败。
    QFile fOut;
    bool bOpened = false;
    QString sCandidate = sOut;
    bool bRenamed = false;
    for (int attempt = 0; attempt < 1000 && !bOpened; ++attempt)
    {
        fOut.setFileName(sCandidate);
        if (fOut.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        {
            bOpened = true;
            break;
        }
        if (!QFileInfo::exists(sCandidate))
            break;   // 不是“已存在”（权限/路径错误），直接报错
        const QFileInfo t(sCandidate);
        const QString sSuffix = t.suffix().isEmpty()
                                    ? QString() : QStringLiteral(".") + t.suffix();
        sCandidate = t.absolutePath() + QStringLiteral("/") + t.completeBaseName()
                     + QStringLiteral(" (") + QString::number(attempt + 1) + QStringLiteral(")")
                     + sSuffix;
        bRenamed = true;
    }
    if (!bOpened)
    {
        const bool bExhausted = QFileInfo::exists(sCandidate);
        setError(bExhausted
            ? tr("目标文件已存在且自动改名达到上限（1000 次），已取消导出：%1").arg(sOut)
            : tr("打开输出文件失败：%1 (%2)").arg(sCandidate, fOut.errorString()));
        return;
    }
    sOut = sCandidate;
    if (bRenamed)
        emit warningOccurred(tr("目标文件已存在，为避免覆盖已自动重命名为：%1").arg(sOut));

    // P1-1：本操作独占的源句柄（不再共享 m_qFile 游标）
    QFile localSrc;
    qint64 srcPos = 0;
    QString sOpenErr;
    if (!openEntrySource(sFilePath, fi, localSrc, srcPos, sOpenErr))
    {
        fOut.close(); QFile::remove(sOut);
        setError(sOpenErr);
        return;
    }

    FwArc::Crc32 crc;
    qint64 copiedTotal = 0;
    QString err;
    const bool bOk = streamCopy(localSrc, srcPos, fi.nFileLen, fOut, crc,
                                copiedTotal, qMax<qint64>(fi.nFileLen, 1),
                                tr("正在导出: %1").arg(fi.sFileName), err);
    localSrc.close();
    fOut.close();

    if (!bOk)
    {
        QFile::remove(sOut);
        setError(tr("导出失败：%1 — %2").arg(sFilePath, err));
        return;
    }
    if (m_mapContentCrc.contains(sFilePath) && crc.value() != m_mapContentCrc.value(sFilePath))
    {
        QFile::remove(sOut);
        setError(tr("导出失败：内容校验不通过（CRC32 不匹配），%1 可能已损坏。").arg(sFilePath));
        return;
    }
    // 把条目录下的文件时间/属性写回导出文件；失败只告警，内容已经成功写出。
    if (fi.bHasFsMetadata)
    {
        QString sMetaErr;
        if (!FwMeta::apply(sOut, fi.nCreatedTime, fi.nModifiedTime, fi.nAccessedTime,
                           fi.nAttributes, &sMetaErr))
        {
            emit warningOccurred(tr("文件已导出，但时间/属性恢复失败：%1\n%2").arg(sOut, sMetaErr));
        }
    }
    m_sLastError.clear();
    m_sLastOutputPath = sOut;
}

// ============================================================
// 显式校验归档（P1-7）
// ============================================================

bool FileManager::verifyArchive(QString *psReport)
{
    if (!isArchiveLoaded())
    {
        if (psReport) *psReport = tr("没有已加载的归档文件");
        return false;
    }
    BusyGuard guard(this);

    QFile f(m_qFile.fileName());
    if (!f.open(QIODevice::ReadOnly))
    {
        const QString s = tr("打开归档失败：%1 (%2)").arg(m_qFile.fileName(), f.errorString());
        if (psReport) *psReport = s;
        setError(s);
        return false;
    }

    qint64 totalBytes = 0;
    for (const FwArc::Entry &e : m_meta.entries) totalBytes += qint64(e.size);
    qint64 copiedTotal = 0;
    QByteArray buf;
    buf.resize(int(kCopyChunk));
    int nBad = 0;
    QStringList badList;

    // 报告里的条目标识一律用「归档内路径」（displayPath，与应用内「属性」显示的一致）：
    // 只写文件名时，归档里两个同名 report.docx 会让用户无法判断是哪一条。
    const auto entryPathOf = [](const FwArc::Entry &e) {
        return e.displayPath.isEmpty() ? e.relPath : e.displayPath;
    };

    for (int i = 0; i < m_meta.entries.size(); ++i)
    {
        if (m_bCancelRequested) break;
        const FwArc::Entry &e = m_meta.entries.at(i);
        if (!f.seek(qint64(e.contentPos)))
        {
            ++nBad;
            badList << tr("第 %1 条（定位失败）：%2").arg(i + 1).arg(entryPathOf(e));
            continue;
        }
        FwArc::Crc32 c;
        qint64 done = 0;
        while (done < qint64(e.size))
        {
            if (m_bCancelRequested) break;
            const qint64 n = qMin<qint64>(qint64(e.size) - done, kCopyChunk);
            const qint64 got = f.read(buf.data(), n);
            if (got <= 0) break;
            c.update(buf.data(), got);
            done += got;
            copiedTotal += got;
            emit progressChanged(copiedTotal, totalBytes,
                tr("正在校验: %1  (%2 / %3)")
                   .arg(QFileInfo(e.displayPath).fileName(),
                        FwArc::formatSize(copiedTotal), FwArc::formatSize(totalBytes)));
            QApplication::processEvents();
        }
        if (done != qint64(e.size) || c.value() != e.contentCrc)
        {
            ++nBad;
            badList << tr("第 %1 条：%2").arg(i + 1).arg(entryPathOf(e));
        }
    }
    f.close();

    QString report;
    if (m_bCancelRequested) report = tr("校验已取消");
    else if (nBad == 0)
        report = tr("校验通过：%1 个文件的内容 CRC32 全部匹配。").arg(m_meta.entries.size());
    else
    {
        // 路径较长：逐条独立成行；条数过多时只列前 kMaxBadListed 条并注明剩余数量，
        // 避免几百行的报告把消息框撑到屏幕外（数量本身仍如实给出）。
        const int kMaxBadListed = 20;
        QString sDetail = badList.mid(0, kMaxBadListed).join(QStringLiteral("\n"));
        if (nBad > kMaxBadListed)
            sDetail += tr("\n…（其余 %1 条未列出）").arg(nBad - kMaxBadListed);
        report = tr("校验失败：%1 / %2 个条目的内容校验未通过（CRC32 不匹配或读取失败）。\n\n"
                    "损坏条目：\n%3")
                     .arg(nBad).arg(m_meta.entries.size()).arg(sDetail);
    }
    if (psReport) *psReport = report;
    if (nBad) setError(report);
    return (!m_bCancelRequested && nBad == 0);
}

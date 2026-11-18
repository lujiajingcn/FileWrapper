#include "filemanager.h"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>
#include <QDateTime>
#include <QSet>
#include <algorithm>
#include <cstring>
#include <new>

#define MAXBUFFERSIZE (1024 * 1024 * 16)
static const qint64 kCopyChunk = 1024 * 1024;      // 流式复制缓冲 1 MB
static const qint64 kCrcChunk  = 1024 * 1024;      // 头部区校验缓冲

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
bool    FileManager::isBusy() const { return m_bBusy; }
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

    if (version != FwArc::VERSION_V1 && version != FwArc::VERSION_V2)
        return fail(QStringLiteral("不支持的归档版本: %1").arg(version));
    if (fileCount == 0 || fileCount > FwArc::MAX_FILES)
        return fail(QStringLiteral("文件数量异常: %1").arg(fileCount));
    if (hdrSize < FwArc::GLOBAL_HDR || quint64(hdrSize) > fileSize
        || quint64(hdrSize) > FwArc::MAX_HDR_SIZE)
        return fail(QStringLiteral("头部大小异常: %1").arg(hdrSize));

    const quint32 entryFixed = (version == FwArc::VERSION_V2)
                             ? FwArc::ENTRY_FIXED_V2 : FwArc::ENTRY_FIXED_V1;
    const quint64 entriesRegion = quint64(hdrSize) - FwArc::GLOBAL_HDR;
    const quint64 metaPrefix    = (version == FwArc::VERSION_V2) ? 4 : 0;
    if (entriesRegion < metaPrefix + quint64(entryFixed) * fileCount)
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

    if (version == FwArc::VERSION_V2)
    {
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
    }

    meta.entries.reserve(int(fileCount));
    for (quint32 i = 0; i < fileCount; ++i)
    {
        if (cursor + qint64(entryFixed) > hdrSize)
            return fail(QStringLiteral("第 %1 条记录越界（条目区不足）").arg(i));

        char plBuf[4];
        if (f.read(plBuf, 4) != 4) return fail(QStringLiteral("第 %1 条路径长度读取失败").arg(i));
        const quint32 pathLen = FwArc::readU32(plBuf);
        if (pathLen > FwArc::MAX_PATH_LEN)
            return fail(QStringLiteral("第 %1 条路径长度异常: %2").arg(i).arg(pathLen));
        // v2 允许墓碑条目（快速删除留下的空槽）。判定：路径区首字节为 NUL，
        // PathLength 保持原值，槽位大小不变，后续条目才能正确对齐。
        // PathLength=0 作为旧写法的墓碑一并兼容。v1 不允许空路径。
        if (pathLen == 0 && version != FwArc::VERSION_V2)
            return fail(QStringLiteral("第 %1 条路径长度为 0").arg(i));
        if (cursor + qint64(entryFixed) + pathLen > hdrSize)
            return fail(QStringLiteral("第 %1 条记录越界（路径区不足）").arg(i));

        char fsBuf[8], cpBuf[8];
        if (f.read(fsBuf, 8) != 8) return fail(QStringLiteral("第 %1 条大小读取失败").arg(i));
        if (f.read(cpBuf, 8) != 8) return fail(QStringLiteral("第 %1 条偏移读取失败").arg(i));
        quint32 entryCrc = 0;
        if (version == FwArc::VERSION_V2)
        {
            char crcBuf[4];
            if (f.read(crcBuf, 4) != 4) return fail(QStringLiteral("第 %1 条内容 CRC 读取失败").arg(i));
            entryCrc = FwArc::readU32(crcBuf);
        }
        const quint64 size = FwArc::readU64(fsBuf);
        const quint64 pos  = FwArc::readU64(cpBuf);

        // v2 墓碑：PathLength 保持原值（槽位大小不变，后续条目才能正确对齐），
        // 用路径区首字节为 NUL 作为删除标记。写入端 markDeletedOnly() 与此处一致。
        QByteArray pb(qsizetype(pathLen), Qt::Uninitialized);
        if (f.read(pb.data(), pathLen) != pathLen)
            return fail(QStringLiteral("第 %1 条路径读取失败").arg(i));
        const bool bTomb = (version == FwArc::VERSION_V2) && (pathLen == 0 || pb.at(0) == char(0));
        if (bTomb)
        {
            if (size > FwArc::MAX_CONTENT_LEN)
                return fail(QStringLiteral("第 %1 条（墓碑）文件大小异常").arg(i));
            if (pos < hdrSize || pos > fileSize || size > fileSize - pos)
                return fail(QStringLiteral("第 %1 条（墓碑）内容区越界").arg(i));
            cursor += qint64(entryFixed) + pathLen;
            continue;
        }
        if (!FwArc::isValidUtf8(pb))
            return fail(QStringLiteral("第 %1 条路径不是合法 UTF-8").arg(i));

        const QString path = QString::fromUtf8(pb);

        FwArc::Entry e;
        e.size          = size;
        e.contentPos    = pos;
        e.contentCrc    = entryCrc;
        e.hasContentCrc = (version == FwArc::VERSION_V2);

        if (version == FwArc::VERSION_V2)
        {
            QString why;
            if (!FwArc::isSafeRelativePath(path, &why))
                return fail(QStringLiteral("第 %1 条路径不安全：%2").arg(i).arg(why));
            e.relPath     = path;
            e.displayPath = FwArc::displayPathFor(meta.root, path);
        }
        else
        {
            // v1 允许绝对路径（老归档如 1.dat 全是 G:/...），只做基本字符校验
            if (path.contains(QChar(0)))
                return fail(QStringLiteral("第 %1 条路径含 NUL 字符").arg(i));
            for (int k = 0; k < path.size(); ++k)
                if (path.at(k).unicode() < 32)
                    return fail(QStringLiteral("第 %1 条路径含控制字符").arg(i));
            e.displayPath = path;
            e.relPath     = path;   // 解析完成后统一按公共根收敛
        }

        // —— 条目字段合法性：伪造 fileSize / contentPos 会在这里被拒绝（P1-1）——
        if (size > FwArc::MAX_CONTENT_LEN)
            return fail(QStringLiteral("第 %1 条文件大小异常: %2").arg(i).arg(size));
        if (pos < hdrSize)
            return fail(QStringLiteral("第 %1 条内容偏移落在头部区: %2").arg(i).arg(pos));
        if (pos > fileSize || size > fileSize - pos)
            return fail(QStringLiteral("第 %1 条内容区越界（size=%2 pos=%3 文件=%4）")
                        .arg(i).arg(size).arg(pos).arg(fileSize));

        meta.entries.append(e);
        cursor += qint64(entryFixed) + pathLen;
    }

    // 内容区不得重叠。
    //
    // 注意零长度条目（0 字节文件）：写入端 writeV2Header 按文件长度累加内容偏移，
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

    // v1：由展示路径推导公共根与相对路径，供分割时还原目录层级（P2-1 / P2-8）。
    // v1 只记录绝对路径，跨目录 / 跨盘符时推导出的相对路径会带很深的无意义层级
    // （且换机器后不可靠）；层级 >= 3 时退化为纯文件名，由 chooseOutputPath 的
    // 自动重命名机制解决同名冲突。
    if (version == FwArc::VERSION_V1)
    {
        QStringList all;
        for (const FwArc::Entry &e : meta.entries) all << e.displayPath;
        meta.root = FwArc::commonRoot(all);
        for (FwArc::Entry &e : meta.entries)
        {
            QString rel = FwArc::sanitizeRelativePath(
                        FwArc::makeRelative(e.displayPath, meta.root));
            if (rel.count('/') >= 2)
                rel = FwArc::sanitizeRelativePath(QFileInfo(e.displayPath).fileName());
            e.relPath = rel;
        }
    }
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

bool FileManager::validateMergedFile(QString sFilePath)
{
    return validateMergedFile(sFilePath, nullptr);
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

    QFile *pSrc = nullptr;
    QFile diskSrc;
    qint64 srcPos = 0;
    if (m_setPendingFiles.contains(sDisplayName))
    {
        diskSrc.setFileName(sDisplayName);
        if (!diskSrc.open(QIODevice::ReadOnly))
        {
            sErr = QStringLiteral("读取新增文件失败: %1 (%2)").arg(sDisplayName, diskSrc.errorString());
            return false;
        }
        pSrc = &diskSrc;
    }
    else
    {
        if (!m_qFile.isOpen()) { sErr = QStringLiteral("归档文件未打开"); return false; }
        pSrc = &m_qFile;
        srcPos = fi.nContentPositon;
    }

    FwArc::Crc32 crc;
    qint64 copiedTotal = 0;
    const bool bOk = streamCopy(*pSrc, srcPos, fi.nFileLen, fOut, crc,
                                copiedTotal, qMax<qint64>(fi.nFileLen, 1),
                                QStringLiteral("正在准备: ") + fi.sFileName, sErr);
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
    return true;
}

// ============================================================
// 加载 / 卸载 / 状态重置（P1-5 / P1-6）
// ============================================================

bool FileManager::QLoadMergedFile(QString sFilePath)
{
    // 先在临时结构上做完整解析；任何失败都保持原归档、原视图、原脏状态不变
    FwArc::Meta meta;
    QString err;
    if (!parseFile(sFilePath, meta, err))
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
        m_vtFileInfos.push_back(fi);
        m_mapFileInfos[fi.sFilePath] = fi;
        if (e.hasContentCrc) m_mapContentCrc.insert(fi.sFilePath, e.contentCrc);
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
    if (!isArchiveLoaded() || m_bBusy) return;

    bool bChanged = false;
    for (const QString &fp : vtNewFiles)
    {
        QFileInfo fi(fp);
        if (!fi.exists() || !fi.isFile()) continue;
        if (m_mapFileInfos.contains(fp)) continue;
        if (quint64(fi.size()) > FwArc::MAX_CONTENT_LEN) continue;

        FILEINFO info;
        info.sFilePath       = fp;
        info.sFileName       = getFileName(fp);
        info.nFileLen        = fi.size();
        info.nContentPositon = 0;
        info.nState          = 0;
        m_vtFileInfos.append(info);
        m_mapFileInfos[fp] = info;
        m_setPendingFiles.insert(fp);
        bChanged = true;
    }
    if (bChanged) m_bDirty = true;
}

bool FileManager::deleteFile(const QString &sFilePath)
{
    if (m_bBusy) return false;
    int idx = -1;
    for (int i = 0; i < m_vtFileInfos.size(); ++i)
        if (m_vtFileInfos.at(i).sFilePath == sFilePath) { idx = i; break; }
    if (idx < 0) return false;

    m_vtFileInfos.remove(idx);
    m_mapFileInfos.remove(sFilePath);
    m_setPendingFiles.remove(sFilePath);
    m_bDirty = true;
    return true;
}

bool FileManager::save()
{
    if (!isArchiveLoaded())
    {
        setError(QStringLiteral("没有已加载的归档文件，无法保存"));
        return false;
    }
    if (m_bBusy)
    {
        setError(QStringLiteral("另有文件操作正在进行，请稍后重试"));
        return false;
    }
    if (!m_bDirty) return true;

    if (m_vtFileInfos.isEmpty())
    {
        const QString sArchivePath = m_qFile.fileName();
        m_qFile.close();
        if (!QFile::remove(sArchivePath))
        {
            setError(QStringLiteral("删除空归档失败: %1 (%2)").arg(sArchivePath, QFile(sArchivePath).errorString()));
            m_qFile.setFileName(sArchivePath);
            m_qFile.open(QIODevice::ReadOnly);
            return false;   // m_bDirty 保持 true，改动仍在内存中，可重试
        }
        m_vtFileInfos.clear();
        m_mapFileInfos.clear();
        m_mapContentCrc.clear();
        m_setPendingFiles.clear();
        m_meta = FwArc::Meta();
        m_bDirty = false;
        return true;
    }

    const bool bOk = rewriteArchive();
    if (bOk) m_bDirty = false;
    // 失败时保持 dirty：改动仍在内存中，用户可重试
    return bOk;
}

// ============================================================
// v2 归档写入：头部占位 → 流式复制内容 → 回填 CRC
// ============================================================

bool FileManager::writeV2Header(QFile &fOut, const QVector<FILEINFO> &infos,
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
    rels.reserve(infos.size());
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
        rels.append(rb);
        hs += FwArc::ENTRY_FIXED_V2 + rb.size();
    }
    if (hs < FwArc::GLOBAL_HDR || quint64(hs) > FwArc::MAX_HDR_SIZE
        || quint64(hs) > 0xFFFFFFFFULL)
        return fail(QStringLiteral("归档头部过大: %1 B").arg(hs));

    if (!fOut.seek(0)) return fail(QStringLiteral("定位输出文件失败"));
    if (fOut.write(FwArc::MAGIC, 4) != 4) return fail(QStringLiteral("写入魔数失败"));
    bool ok = FwArc::writeU32(fOut, FwArc::VERSION_V2)
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
        ok = FwArc::writeU32(fOut, quint32(rels.at(i).size()))
          && FwArc::writeU64(fOut, quint64(infos.at(i).nFileLen))
          && FwArc::writeU64(fOut, quint64(cp));
        if (!ok) return fail(QStringLiteral("写入第 %1 条记录失败").arg(i));
        outCrcFieldPos.append(fOut.pos());
        if (!FwArc::writeU32(fOut, 0))    // ContentCrc32 占位
            return fail(QStringLiteral("写入第 %1 条 CRC 字段失败").arg(i));
        if (!rels.at(i).isEmpty() && fOut.write(rels.at(i)) != rels.at(i).size())
            return fail(QStringLiteral("写入第 %1 条路径失败").arg(i));
        outContentPos.append(cp);
        cp += infos.at(i).nFileLen;
    }
    outHeaderSize = hs;
    return true;
}

bool FileManager::finalizeV2Header(QFile &fOut, qint64 headerSize,
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

// 流式复制 [srcPos, srcPos+len)，同时计算内容 CRC、上报进度、响应取消
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
            QApplication::processEvents();
        }
    }
    return true;
}

// ============================================================
// 合并（P0-1 / P1-7 / P2-4 / P3-2）
// ============================================================

void FileManager::QMergeFiles(QVector<QString> vtInputFiles, QString sOutputFile)
{
    if (m_bBusy) { emit operationFinished(false, tr("另有文件操作正在进行")); return; }
    BusyGuard guard(this);

    auto finish = [this](bool ok, const QString &msg) {
        if (!ok) setError(msg);
        emit operationFinished(ok, msg);
    };

    if (vtInputFiles.isEmpty()) { finish(false, tr("没有要合并的文件")); return; }
    if (sOutputFile.isEmpty())  { finish(false, tr("输出路径为空")); return; }

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

    // ---- 收集输入：已合并的 .dat（FWDA）先拆分成条目，再一起合并 ----
    // 把 .dat 当成普通文件嵌进输出会变成"归档套归档"，用户还要再拆一层；
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
                // 用来源归档的 root 还原它原本的完整路径；手工构造的归档可能没有 root
                s.sOriginPath  = meta.root.isEmpty()
                                     ? e.relPath
                                     : meta.root + QLatin1Char('/') + e.relPath;
                s.sDisplayName = QFileInfo(e.relPath).fileName();
                s.bFromArchive = true;
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
        int nRenamed = 0;
        for (MergeSource &s : sources)
        {
            const QString base = s.sStoredPath;
            QString candidate = base;
            int n = 1;
            while (used.contains(candidate.toLower()))
            {
                ++n;
                const int slash = base.lastIndexOf(QLatin1Char('/'));
                const QString dir  = slash >= 0 ? base.left(slash + 1) : QString();
                const QString name = slash >= 0 ? base.mid(slash + 1) : base;
                const int dot = name.lastIndexOf(QLatin1Char('.'));
                const QString stem = dot > 0 ? name.left(dot) : name;
                const QString ext  = dot > 0 ? name.mid(dot) : QString();
                candidate = dir + stem + QStringLiteral(" (%1)").arg(n) + ext;
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
        infos.append(info);
        storedPaths << s.sStoredPath;
        totalBytes += s.nLength;
    }

    const qint64 rootBytes = qint64(sRoot.toUtf8().size());
    qint64 needBytes = FwArc::GLOBAL_HDR + 4 + rootBytes
                     + FwArc::ENTRY_FIXED_V2 * infos.size() + totalBytes;
    for (const QString &s : storedPaths)
        needBytes += s.toUtf8().size();

    // 磁盘空间预检（P3-2）
    const qint64 freeBytes = FwArc::availableBytes(sOutputFile);
    if (freeBytes >= 0 && freeBytes < needBytes)
    {
        finish(false, tr("磁盘空间不足：需要 %1，可用 %2")
               .arg(FwArc::formatSize(needBytes), FwArc::formatSize(freeBytes)));
        return;
    }

    // 始终写临时文件，成功并校验后才原子替换（P0-1 第 3 点）
    const QString sTmp = sOutputFile + QStringLiteral(".tmp.fwda");
    QFile::remove(sTmp);
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
    if (!writeV2Header(fTmp, infos, sRoot, crcFieldPos, contentPos, headerSize, err,
                       &storedPaths))
    {
        fTmp.close(); QFile::remove(sTmp);
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
        FwArc::Crc32 eCrc;
        bOk = streamCopy(src, s.nOffset, s.nLength, fTmp, eCrc,
                         copiedTotal, totalBytes,
                         tr("正在合并: %1").arg(s.sOriginPath), err);
        contentCrcs.append(eCrc.value());
        src.close();
    }
    if (bOk) bOk = finalizeV2Header(fTmp, headerSize, crcFieldPos, contentCrcs, err);
    fTmp.close();

    if (bOk)
    {
        FwArc::Meta meta;
        bOk = parseFile(sTmp, meta, err);   // 完整解析 + 所有条目合法性校验
        if (!bOk) err = tr("临时归档校验失败：%1").arg(err);
    }
    if (!bOk)
    {
        QFile::remove(sTmp);
        finish(false, m_bCancelRequested ? tr("操作已取消") : tr("文件合并失败：%1").arg(err));
        return;
    }

    // 原子替换（先备份，失败可还原），给出具体系统错误
    const QString sBak = sOutputFile + QStringLiteral(".bak.fwda");
    QFile::remove(sBak);
    if (QFileInfo::exists(sOutputFile))
    {
        if (!QFile::rename(sOutputFile, sBak))
        {
            QFile::remove(sTmp);
            finish(false, tr("备份已有输出文件失败：%1\n文件可能被其它程序占用。").arg(sOutputFile));
            return;
        }
    }
    if (!QFile::rename(sTmp, sOutputFile))
    {
        if (QFileInfo::exists(sBak)) QFile::rename(sBak, sOutputFile);
        QFile::remove(sTmp);
        finish(false, tr("替换输出文件失败：%1 (%2)")
               .arg(sOutputFile, QFile(sTmp).errorString()));
        return;
    }
    QFile::remove(sBak);

    QString sDone = tr("文件合并成功：共 %1 个文件，%2")
                        .arg(sources.size()).arg(FwArc::formatSize(totalBytes));
    if (nArchiveEntries > 0)
        sDone += tr("\n其中 %1 个条目来自 %2 个已合并归档（已先拆分再合并，不是嵌套归档）。")
                     .arg(nArchiveEntries).arg(nArchiveInputs);
    if (nRenamedTotal > 0)
        sDone += tr("\n另有 %1 个条目路径重复，已自动加 \" (2)\" 等后缀改名。")
                     .arg(nRenamedTotal);
    finish(true, sDone);
}

// ============================================================
// 保存：重写为 v2 格式 + 内容 CRC + 空间预检 + 具体失败原因
// ============================================================

// v2 墓碑式快速删除：只改写头部区的路径标记与 HeaderChecksum，
// 内容区一个字节都不动。对 10 GB 归档删除一个 94 B 条目，从「重写 10 GB」
// 变成「改写几十 KB 头部」（P1-3）。
//
// 事务性：头部区一次性读入内存（有上限），在内存里打标记 + 重算 CRC，
// 最后一次连续写回；任何失败都用原头部整体回滚，绝不留下「标记已改、
// CRC 未更新」的半写归档。
bool FileManager::markDeletedOnly(QString &sErr)
{
    const QString sArchivePath = m_qFile.fileName();
    QFile f(sArchivePath);

    auto reopenOwn = [&]() {
        if (!m_qFile.isOpen() && !sArchivePath.isEmpty()) {
            m_qFile.setFileName(sArchivePath);
            m_qFile.open(QIODevice::ReadOnly);
        }
    };
    auto fail = [&](const QString &m) -> bool {
        if (f.isOpen()) f.close();
        reopenOwn();
        sErr = m;
        return false;
    };

    if (sArchivePath.isEmpty()) return fail(QStringLiteral("归档路径为空"));

    // Windows 下先关掉自身只读句柄，避免与写入句柄共享冲突
    if (m_qFile.isOpen()) m_qFile.close();
    if (!f.open(QIODevice::ReadWrite))
        return fail(QStringLiteral("无法以读写方式打开归档: %1").arg(f.errorString()));

    const qint64 fileSize = f.size();
    char gh[FwArc::GLOBAL_HDR];
    if (!f.seek(0) || f.read(gh, FwArc::GLOBAL_HDR) != FwArc::GLOBAL_HDR)
        return fail(QStringLiteral("读取全局头失败"));
    if (std::memcmp(gh, FwArc::MAGIC, 4) != 0)
        return fail(QStringLiteral("魔数不匹配"));
    const quint32 version = FwArc::readU32(gh + 4);
    const quint32 hdrSize = FwArc::readU32(gh + 8);
    const quint32 count   = FwArc::readU32(gh + 12);
    if (version != FwArc::VERSION_V2) return fail(QStringLiteral("不是 v2 归档"));
    if (hdrSize < FwArc::GLOBAL_HDR + 4 || quint64(hdrSize) > quint64(fileSize)
        || quint64(hdrSize) > FwArc::MAX_HDR_SIZE)
        return fail(QStringLiteral("头部大小异常"));
    // 头部过大就不走内存路径，回退流式完整重写
    static const qint64 kFastMaxHdr = 64LL * 1024 * 1024;
    if (qint64(hdrSize) > kFastMaxHdr)
        return fail(QStringLiteral("头部区过大，不适合快速删除"));

    QByteArray hdr(qsizetype(hdrSize), Qt::Uninitialized);
    if (!f.seek(0) || f.read(hdr.data(), hdrSize) != hdrSize)
        return fail(QStringLiteral("回读头部区失败"));
    const QByteArray hdrOrig = hdr;   // 隐式共享，回滚时使用的原始快照

    auto rollback = [&]() -> bool {
        if (!f.isOpen() && !f.open(QIODevice::ReadWrite)) return false;
        if (!f.seek(0)) { f.close(); return false; }
        const bool ok = (f.write(hdrOrig) == hdrOrig.size() && f.flush());
        f.close();
        return ok;
    };

    const char *p = hdr.constData();
    qint64 cursor = FwArc::GLOBAL_HDR;
    const quint32 rootLen = FwArc::readU32(p + cursor);
    if (rootLen > FwArc::MAX_ROOT_LEN || cursor + 4 + qint64(rootLen) > hdrSize)
        return fail(QStringLiteral("原始根长度异常"));
    const QString root = QString::fromUtf8(p + cursor + 4, qsizetype(rootLen));
    cursor += 4 + qint64(rootLen);

    QSet<QString> keep;
    for (const FILEINFO &fi : m_vtFileInfos) keep.insert(fi.sFilePath);
    if (keep.isEmpty() || count > FwArc::MAX_FILES)
        return fail(QStringLiteral("条目数异常，回退完整重写"));

    QVector<qint64> tombPathPos;
    int matched = 0;
    for (quint32 i = 0; i < count; ++i)
    {
        if (cursor + qint64(FwArc::ENTRY_FIXED_V2) > hdrSize)
            return fail(QStringLiteral("条目区越界"));
        const quint32 pathLen = FwArc::readU32(p + cursor);
        if (pathLen > FwArc::MAX_PATH_LEN
            || cursor + qint64(FwArc::ENTRY_FIXED_V2) + pathLen > hdrSize)
            return fail(QStringLiteral("条目区与路径长度不匹配"));
        const qint64 pathPos = cursor + FwArc::ENTRY_FIXED_V2;
        const char *pPath = p + pathPos;
        // 已是墓碑的槽位跳过；PathLength=0 是旧写法的墓碑，保持原样
        if (pathLen > 0 && pPath[0] != char(0))
        {
            const QString display = FwArc::displayPathFor(
                        root, QString::fromUtf8(pPath, qsizetype(pathLen)));
            if (keep.contains(display)) ++matched;
            else tombPathPos.append(pathPos);
        }
        cursor += qint64(FwArc::ENTRY_FIXED_V2) + pathLen;
    }
    if (tombPathPos.isEmpty())
        return fail(QStringLiteral("未找到需要标记删除的条目"));
    if (matched != keep.size())
        return fail(QStringLiteral("待保留条目与归档头部不一致，回退完整重写"));
    if (tombPathPos.size() >= int(count))
        return fail(QStringLiteral("拒绝把全部条目标记为删除，回退完整重写"));

    p = nullptr;   // 下面会写 hdr，触发与 hdrOrig 的分离，原指针不再使用

    // 打墓碑：只把路径区首字节改成 NUL，PathLength 与槽位大小不变，
    // 这样后续条目仍按原 PathLength 对齐（parseFromDevice 用同一规则识别）。
    for (qint64 pos : tombPathPos) hdr[qsizetype(pos)] = char(0);

    // HeaderChecksum = 全局头[0..15] + [20..23] + 头部区[24..hdrSize)
    FwArc::Crc32 c;
    c.update(hdr.constData(), 16);
    c.update(hdr.constData() + 20, 4);
    c.update(hdr.constData() + FwArc::GLOBAL_HDR, qint64(hdrSize) - FwArc::GLOBAL_HDR);
    const quint32 crc = c.value();
    for (int k = 0; k < 4; ++k) hdr[16 + k] = char((crc >> (8 * k)) & 0xFFu);

    if (!f.seek(0) || f.write(hdr) != hdr.size() || !f.flush())
    {
        rollback();
        return fail(QStringLiteral("写回头部区失败"));
    }
    f.close();

    // 写回后必须能按新头部完整解析，且存活条目数与预期一致
    FwArc::Meta chk;
    QString perr;
    if (!parseFile(sArchivePath, chk, perr) || chk.entries.size() != keep.size())
    {
        rollback();
        return fail(QStringLiteral("快速删除后头部校验失败，已回滚：%1").arg(perr));
    }

    QLoadMergedFile(sArchivePath);   // 刷新偏移、清 dirty / pending，并重开自身句柄
    if (!isArchiveLoaded())
    {
        rollback();
        return fail(QStringLiteral("重新加载归档失败"));
    }
    return true;
}

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
    const QVector<FILEINFO> infos = m_vtFileInfos;

    // 只有「删除」且归档已是 v2 时走快速路径；新增文件仍需完整重写（顺带清理墓碑）
    if (m_meta.version == FwArc::VERSION_V2
        && m_setPendingFiles.isEmpty()
        && infos.size() < m_meta.entries.size())
    {
        QString fastErr;
        if (markDeletedOnly(fastErr)) return true;
        qWarning() << "[FileManager] 快速删除不可用，回退完整重写:" << fastErr;
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
        totalBytes += fi.nFileLen;

    // 头部按实际存储路径精确计算（含每条路径字节），避免空间预检偏小
    qint64 headerBytes = FwArc::GLOBAL_HDR + 4 + qint64(sRoot.toUtf8().size());
    for (const QString &rel : storedPaths)
        headerBytes += FwArc::ENTRY_FIXED_V2 + rel.toUtf8().size();
    const qint64 needBytes = headerBytes + totalBytes;
    const qint64 freeBytes = FwArc::availableBytes(sArchivePath);
    if (freeBytes >= 0 && freeBytes < needBytes)
    {
        setError(tr("磁盘空间不足，无法保存：需要 %1，可用 %2")
                 .arg(FwArc::formatSize(needBytes), FwArc::formatSize(freeBytes)));
        return false;
    }

    const QString sTmp = sArchivePath + QStringLiteral(".tmp.fwda");
    QFile::remove(sTmp);
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
    if (!writeV2Header(fTmp, infos, sRoot, crcFieldPos, contentPos, headerSize, err,
                       &storedPaths))
    {
        fTmp.close(); QFile::remove(sTmp);
        setError(tr("写入归档头部失败：%1").arg(err));
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
        if (m_setPendingFiles.contains(fi.sFilePath))
        {
            QFile src(fi.sFilePath);
            if (!src.open(QIODevice::ReadOnly))
            {
                err = tr("读取新增文件失败：%1 (%2)").arg(fi.sFilePath, src.errorString());
                bOk = false; break;
            }
            bOk = streamCopy(src, 0, fi.nFileLen, fTmp, eCrc, copiedTotal, totalBytes,
                             tr("正在保存: %1").arg(fi.sFileName), err);
            src.close();
        }
        else
        {
            bOk = streamCopy(m_qFile, fi.nContentPositon, fi.nFileLen, fTmp, eCrc,
                             copiedTotal, totalBytes,
                             tr("正在保存: %1").arg(fi.sFileName), err);
        }
        contentCrcs.append(eCrc.value());
    }
    if (bOk) bOk = finalizeV2Header(fTmp, headerSize, crcFieldPos, contentCrcs, err);
    fTmp.close();

    if (bOk)
    {
        FwArc::Meta meta;
        bOk = parseFile(sTmp, meta, err);
        if (!bOk) err = tr("临时归档校验失败：%1").arg(err);
    }
    if (!bOk)
    {
        QFile::remove(sTmp);
        setError(m_bCancelRequested ? tr("保存已取消")
                                    : tr("保存失败：%1\n改动仍保留在内存中，可重试。").arg(err));
        return false;
    }

    // Windows：必须先关闭自身句柄，否则 rename 会因共享冲突失败
    m_qFile.close();

    const QString sBak = sArchivePath + QStringLiteral(".bak.fwda");
    QFile::remove(sBak);
    if (!QFile::rename(sArchivePath, sBak))
    {
        QFile::remove(sTmp);
        m_qFile.setFileName(sArchivePath);
        m_qFile.open(QIODevice::ReadOnly);   // 保留内存状态与 dirty，不重新解析
        setError(tr("备份原归档失败：%1\n文件可能被其它程序（资源管理器、杀毒软件或另一个实例）占用。"
                    "\n改动仍保留在内存中，请关闭占用后重试。").arg(sArchivePath));
        return false;
    }
    if (!QFile::rename(sTmp, sArchivePath))
    {
        QFile::rename(sBak, sArchivePath);
        m_qFile.setFileName(sArchivePath);
        m_qFile.open(QIODevice::ReadOnly);
        setError(tr("替换原归档失败：%1\n改动仍保留在内存中，可重试。").arg(sArchivePath));
        return false;
    }
    QFile::remove(sBak);

    // 重新解析刷新偏移，并顺带清掉 dirty / pending
    QLoadMergedFile(sArchivePath);
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
        const QString p = FwArc::normalizePath(e.displayPath);
        const QStringList segs = p.split('/', Qt::SkipEmptyParts);
        for (const QString &seg : segs)
            if (seg == "." || seg == "..") return QString();   // 拒绝回溯
        if (p.isEmpty() || FwArc::isReservedDeviceName(segs.isEmpty() ? p : segs.last()))
            return QString();
        for (int i = 0; i < p.size(); ++i)
            if (p.at(i).unicode() < 32) return QString();
        target = p;
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
    if (m_bBusy) { emit operationFinished(false, tr("另有文件操作正在进行")); return; }
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
    for (const FwArc::Entry &e : meta.entries) totalBytes += qint64(e.size);
    qint64 copiedTotal = 0;
    QSet<QString> setUsed;
    int nRenamed = 0, nSkipped = 0, nFailed = 0, nDone = 0, nCrcBad = 0;

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
            qWarning() << "创建输出目录失败:" << sDir;
            ++nFailed; continue;
        }

        QFile fOut(sOut);
        if (!fOut.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            qWarning() << "打开输出文件失败:" << sOut << fOut.errorString();
            ++nFailed; continue;
        }

        if (!fIn.seek(qint64(e.contentPos)))
        {
            fOut.close(); QFile::remove(sOut);
            ++nFailed; continue;
        }

        FwArc::Crc32 crc;
        qint64 done = 0;
        bool bOk = true;
        int nLastPercent = -1;
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
                emit progressChanged(copiedTotal, totalBytes,
                    tr("正在分割: %1  (%2 / %3)")
                       .arg(QFileInfo(sOut).fileName(), FwArc::formatSize(copiedTotal),
                            FwArc::formatSize(totalBytes)));
                QApplication::processEvents();
            }
        }
        fOut.close();

        if (!bOk || done != qint64(e.size))
        {
            QFile::remove(sOut);
            ++nFailed;
            continue;
        }
        // 内容 CRC 校验（v1 无校验字段，跳过但会在汇总里说明）
        if (e.hasContentCrc && crc.value() != e.contentCrc)
        {
            QFile::remove(sOut);
            ++nCrcBad;
            emit warningOccurred(tr("第 %1 个文件内容校验失败（CRC32 不匹配），已删除输出：%2")
                                .arg(i + 1).arg(sOut));
            continue;
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
    if (meta.version == FwArc::VERSION_V1)
        msg += tr("（v1 归档无内容校验字段，未做逐条 CRC 校验）");
    finish(nFailed == 0 && nCrcBad == 0, msg);
}

// ============================================================
// 读取与导出（P1-2 / P1-7 / P3-2）
// ============================================================

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

    QFile *pSrc = nullptr;
    QFile  diskSrc;
    qint64 srcPos = 0;
    if (m_setPendingFiles.contains(sFilePath))
    {
        diskSrc.setFileName(sFilePath);
        if (!diskSrc.open(QIODevice::ReadOnly))
        {
            if (psErr) *psErr = tr("读取新增文件失败：%1 (%2)").arg(sFilePath, diskSrc.errorString());
            return QByteArray();
        }
        pSrc = &diskSrc;
    }
    else
    {
        if (!m_qFile.isOpen())
        {
            if (psErr) *psErr = tr("归档文件未打开");
            return QByteArray();
        }
        pSrc = &m_qFile;
        srcPos = fi.nContentPositon;
    }

    const qint64 nWant = qMin<qint64>(fi.nFileLen, nMaxBytes);
    if (!pSrc->seek(srcPos))
    {
        if (psErr) *psErr = tr("定位文件内容失败：%1 (%2)").arg(sFilePath, pSrc->errorString());
        return QByteArray();
    }

    QByteArray out;
    out.resize(qsizetype(nWant));
    qint64 done = 0;
    while (done < nWant)
    {
        const qint64 n = qMin<qint64>(nWant - done, MAXBUFFERSIZE);
        const qint64 got = pSrc->read(out.data() + done, n);
        if (got <= 0) break;
        done += got;
    }
    out.resize(qsizetype(done));
    return out;
}

// C2：直接读出条目内容到 QByteArray，省掉"new char[] -> 插件再复制一份"的中转。
// 与 QGetFileContent() 的校验语义保持一致（大小、完整性、v2 内容 CRC）；
// 两者共用同一套归档/待保存文件解析逻辑，改动时请同步。
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

    QFile *pSrc = nullptr;
    QFile  diskSrc;
    qint64 srcPos = 0;
    if (m_setPendingFiles.contains(sFilePath))
    {
        diskSrc.setFileName(sFilePath);
        if (!diskSrc.open(QIODevice::ReadOnly))
        {
            if (psErr) *psErr = tr("读取新增文件失败：%1 (%2)").arg(sFilePath, diskSrc.errorString());
            return QByteArray();
        }
        pSrc = &diskSrc;
        srcPos = 0;
    }
    else
    {
        if (!m_qFile.isOpen())
        {
            if (psErr) *psErr = tr("归档文件未打开");
            return QByteArray();
        }
        pSrc = &m_qFile;
        srcPos = fi.nContentPositon;
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

    if (!pSrc->seek(srcPos))
    {
        if (psErr) *psErr = tr("定位文件内容失败：%1 (%2)").arg(sFilePath, pSrc->errorString());
        return QByteArray();
    }

    qint64 done = 0;
    while (done < fi.nFileLen)
    {
        const qint64 n = qMin<qint64>(fi.nFileLen - done, MAXBUFFERSIZE);
        const qint64 got = pSrc->read(out.data() + done, n);
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

void FileManager::QGetFileContent(QString sFilePath, char **szBuf, qint64 &nFileLen)
{
    *szBuf = nullptr;
    nFileLen = 0;

    auto it = m_mapFileInfos.find(sFilePath);
    if (it == m_mapFileInfos.end())
    {
        setError(tr("归档内未找到文件：%1").arg(sFilePath));
        return;
    }
    const FILEINFO fi = it.value();
    if (fi.nFileLen < 0) { setError(tr("文件大小异常：%1").arg(sFilePath)); return; }
    if (fi.nFileLen == 0)
    {
        // 0 字节文件是合法情况，不视为读取失败（P3-2）
        m_sLastError.clear();
        return;
    }

    QFile *pSrc = nullptr;
    QFile  diskSrc;
    qint64 srcPos = 0;
    if (m_setPendingFiles.contains(sFilePath))
    {
        diskSrc.setFileName(sFilePath);
        if (!diskSrc.open(QIODevice::ReadOnly))
        {
            setError(tr("读取新增文件失败：%1 (%2)").arg(sFilePath, diskSrc.errorString()));
            return;
        }
        pSrc = &diskSrc;
        srcPos = 0;
    }
    else
    {
        if (!m_qFile.isOpen()) { setError(tr("归档文件未打开")); return; }
        pSrc = &m_qFile;
        srcPos = fi.nContentPositon;
    }

    // std::nothrow：畸形头部或超大文件不再让进程因 bad_alloc 终止（P1-1）
    char *pBuf = new (std::nothrow) char[qsizetype(fi.nFileLen)];
    if (!pBuf)
    {
        setError(tr("内存不足，无法一次性读取 %1（%2）。请改用导出或分割功能。")
                 .arg(fi.sFileName, FwArc::formatSize(fi.nFileLen)));
        return;
    }

    // 分块读取，避免一次 read 超大长度
    if (!pSrc->seek(srcPos))
    {
        delete[] pBuf;
        setError(tr("定位文件内容失败：%1 (%2)").arg(sFilePath, pSrc->errorString()));
        return;
    }
    qint64 done = 0;
    bool bOk = true;
    while (done < fi.nFileLen)
    {
        const qint64 n = qMin<qint64>(fi.nFileLen - done, MAXBUFFERSIZE);
        const qint64 got = pSrc->read(pBuf + done, n);
        if (got <= 0) { bOk = false; break; }
        done += got;
    }
    if (!bOk || done != fi.nFileLen)
    {
        delete[] pBuf;
        setError(tr("读取文件内容不完整：%1（期望 %2 B，实际 %3 B）")
                 .arg(sFilePath).arg(fi.nFileLen).arg(done));
        return;
    }

    if (m_mapContentCrc.contains(sFilePath))
    {
        FwArc::Crc32 c;
        c.update(pBuf, fi.nFileLen);
        if (c.value() != m_mapContentCrc.value(sFilePath))
        {
            delete[] pBuf;
            setError(tr("内容校验失败（CRC32 不匹配）：%1\n该文件可能已损坏。").arg(sFilePath));
            return;
        }
    }

    *szBuf = pBuf;
    nFileLen = fi.nFileLen;
    m_sLastError.clear();
}

void FileManager::outputFile(QString sFilePath, QString sOutputDir)
{
    m_sLastError.clear();
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
        sOut = sFilePath;   // 「按原路径导出」：仍做覆盖保护
    }

    // 绝不静默覆盖已有文件
    if (QFileInfo::exists(sOut))
    {
        const QFileInfo t(sOut);
        const QString dir = t.absolutePath();
        const QString base = t.completeBaseName();
        const QString ext = t.suffix();
        QString cand;
        int n = 1;
        do {
            cand = dir + "/" + base + " (" + QString::number(n) + ")"
                 + (ext.isEmpty() ? QString() : "." + ext);
            ++n;
        } while (QFileInfo::exists(cand) && n < 100000);
        sOut = cand;
        emit warningOccurred(tr("目标文件已存在，为避免覆盖已自动重命名为：%1").arg(sOut));
    }

    QFile fOut(sOut);
    if (!fOut.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        setError(tr("打开输出文件失败：%1 (%2)").arg(sOut, fOut.errorString()));
        return;
    }

    QFile *pSrc = nullptr;
    QFile diskSrc;
    qint64 srcPos = 0;
    if (m_setPendingFiles.contains(sFilePath))
    {
        diskSrc.setFileName(sFilePath);
        if (!diskSrc.open(QIODevice::ReadOnly))
        {
            fOut.close(); QFile::remove(sOut);
            setError(tr("读取新增文件失败：%1 (%2)").arg(sFilePath, diskSrc.errorString()));
            return;
        }
        pSrc = &diskSrc; srcPos = 0;
    }
    else
    {
        if (!m_qFile.isOpen())
        {
            fOut.close(); QFile::remove(sOut);
            setError(tr("归档文件未打开")); return;
        }
        pSrc = &m_qFile; srcPos = fi.nContentPositon;
    }

    FwArc::Crc32 crc;
    qint64 copiedTotal = 0;
    QString err;
    const bool bOk = streamCopy(*pSrc, srcPos, fi.nFileLen, fOut, crc,
                                copiedTotal, qMax<qint64>(fi.nFileLen, 1),
                                tr("正在导出: %1").arg(fi.sFileName), err);
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
    m_sLastError.clear();
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

    if (m_meta.version == FwArc::VERSION_V1)
    {
        const QString s = tr("当前归档是 v1 格式，不含逐条目内容校验字段，无法检测内容区损坏。"
                             "\n建议用「保存」重写为 v2 格式后即可校验。");
        if (psReport) *psReport = s;
        setError(s);
        return false;
    }

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

    for (int i = 0; i < m_meta.entries.size(); ++i)
    {
        if (m_bCancelRequested) break;
        const FwArc::Entry &e = m_meta.entries.at(i);
        if (!f.seek(qint64(e.contentPos))) { ++nBad; badList << tr("第 %1 条（定位失败）").arg(i + 1); continue; }
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
            badList << tr("第 %1 条 %2").arg(i + 1).arg(QFileInfo(e.displayPath).fileName());
        }
    }
    f.close();

    QString report;
    if (m_bCancelRequested) report = tr("校验已取消");
    else if (nBad == 0)
        report = tr("校验通过：%1 个文件的内容 CRC32 全部匹配。").arg(m_meta.entries.size());
    else
        report = tr("校验失败：%1 / %2 个文件内容不一致。\n损坏条目：%3")
                 .arg(nBad).arg(m_meta.entries.size()).arg(badList.join(QStringLiteral("、")));
    if (psReport) *psReport = report;
    if (nBad) setError(report);
    return (!m_bCancelRequested && nBad == 0);
}

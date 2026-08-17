#include "filemanager.h"
#include <QApplication>
#include <QDebug>
#include <QByteArray>
#include <QDir>
#include <cstring>
#include <cstdio>

#define MAXBUFFERSIZE 1024 * 1024 * 4

// ============================================================
// 归档格式（二进制，小端）
//
// 全局头部 24 字节:
//   [0..3]   Magic "FWDA"
//   [4..7]   Version  uint32 当前为 1
//   [8..11]  HeaderSize uint32 全局头+条目头的总大小
//   [12..15] FileCount  uint32
//   [16..19] HeaderChecksum uint32 CRC32
//   [20..23] Reserved   uint32 (0)
//
// 文件条目 (重复 FileCount 次):
//   [0..3]   PathLength  uint32
//   [4..11]  FileSize    uint64
//   [12..19] ContentPos  uint64
//   [20..]   Path        uint8[PathLength] (UTF-8)
// ============================================================
//
// 注意：条目固定部分为 20 字节（FWDAT_ENTRY_FIXED），
// 此前的注释曾误写为 16。

static const char FWDAT_MAGIC[4] = {'F', 'W', 'D', 'A'};
static const quint32 FWDAT_VERSION = 1;
static const quint32 FWDAT_GLOBAL_HDR = 24;
static const quint32 FWDAT_ENTRY_FIXED = 20;
static const quint32 FWDAT_MAX_FILES = 1000000;

// ---------- 内联小端读写 ----------

static inline quint32 readU32(const char *b)
{
    return quint32(quint8(b[0]))
         | quint32(quint8(b[1])) << 8
         | quint32(quint8(b[2])) << 16
         | quint32(quint8(b[3])) << 24;
}

static inline quint64 readU64(const char *b)
{
    quint64 v = 0;
    for (int i = 0; i < 8; i++)
        v |= quint64(quint8(b[i])) << (i * 8);
    return v;
}

static inline void writeU32(QFile &f, quint32 v)
{
    char buf[4];
    buf[0] = char(v & 0xFF);
    buf[1] = char((v >> 8) & 0xFF);
    buf[2] = char((v >> 16) & 0xFF);
    buf[3] = char((v >> 24) & 0xFF);
    f.write(buf, 4);
}

static inline void writeU64(QFile &f, quint64 v)
{
    char buf[8];
    for (int i = 0; i < 8; i++)
        buf[i] = char((v >> (i * 8)) & 0xFF);
    f.write(buf, 8);
}

// 向 QByteArray 末尾追加小端 uint32
static inline void appendU32(QByteArray &a, quint32 v)
{
    char buf[4];
    buf[0] = char(v & 0xFF);
    buf[1] = char((v >> 8) & 0xFF);
    buf[2] = char((v >> 16) & 0xFF);
    buf[3] = char((v >> 24) & 0xFF);
    a.append(buf, 4);
}

// 向 QByteArray 末尾追加小端 uint64
static inline void appendU64(QByteArray &a, quint64 v)
{
    char buf[8];
    for (int i = 0; i < 8; i++)
        buf[i] = char((v >> (i * 8)) & 0xFF);
    a.append(buf, 8);
}

// ---------- CRC32 ----------

static quint32 computeCrc32(const QByteArray &data)
{
    static quint32 crcTable[256];
    static bool tableReady = false;
    if (!tableReady)
    {
        for (quint32 i = 0; i < 256; i++)
        {
            quint32 c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            crcTable[i] = c;
        }
        tableReady = true;
    }
    quint32 crc = 0xFFFFFFFFu;
    for (int i = 0; i < data.size(); i++)
        crc = crcTable[(crc ^ quint8(data.at(i))) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

// 人类可读的字节大小，用于进度条状态文本
static QString formatSize(qint64 bytes)
{
    const qint64 kb = 1024;
    const qint64 mb = 1024 * 1024;
    const qint64 gb = 1024LL * 1024 * 1024;
    if (bytes >= gb)
        return QString::number(double(bytes) / gb, 'f', 2) + " GB";
    if (bytes >= mb)
        return QString::number(double(bytes) / mb, 'f', 2) + " MB";
    if (bytes >= kb)
        return QString::number(double(bytes) / kb, 'f', 2) + " KB";
    return QString::number(bytes) + " B";
}

// ---------- 头部校验 ----------

static bool readAndValidateHeader(QFile &f, quint32 &outFileCount,
                                  quint32 &outHeaderSize,
                                  quint32 &outContentStart)
{
    outFileCount = 0;
    outHeaderSize = 0;
    outContentStart = 0;

    if (!f.seek(0))
    {
        qWarning() << "定位合并文件失败";
        return false;
    }

    // 1) 读全局头部 24 字节
    char globalHdr[FWDAT_GLOBAL_HDR];
    if (f.read(globalHdr, FWDAT_GLOBAL_HDR) != FWDAT_GLOBAL_HDR)
    {
        qWarning() << "读取文件头失败";
        return false;
    }

    // 2) 校验魔数
    if (memcmp(globalHdr, FWDAT_MAGIC, 4) != 0)
    {
        qWarning() << "不是有效的归档文件（魔数不匹配）";
        return false;
    }

    quint32 version    = readU32(globalHdr + 4);
    quint32 hdrSize    = readU32(globalHdr + 8);
    quint32 fileCount  = readU32(globalHdr + 12);
    quint32 storedCrc  = readU32(globalHdr + 16);

    // 3) 版本检查
    if (version != FWDAT_VERSION)
    {
        qWarning() << "不支持的归档版本:" << version;
        return false;
    }

    // 4) 文件数量合理性
    if (fileCount == 0 || fileCount > FWDAT_MAX_FILES)
    {
        qWarning() << "文件数量异常:" << fileCount;
        return false;
    }

    // 5) HeaderSize 合理性
    if (hdrSize < FWDAT_GLOBAL_HDR)
    {
        qWarning() << "头部大小异常:" << hdrSize;
        return false;
    }
    quint32 entriesRegion = hdrSize - FWDAT_GLOBAL_HDR;
    quint32 minEntriesSize = FWDAT_ENTRY_FIXED * fileCount;
    if (entriesRegion < minEntriesSize || entriesRegion > 512 * 1024 * 1024)
    {
        qWarning() << "头部大小与文件数量不匹配";
        return false;
    }

    // 6) 文件总长度必须 >= hdrSize
    if (quint64(hdrSize) > f.size())
    {
        qWarning() << "文件长度不足以容纳头部";
        return false;
    }

    // 7) 读取全部条目头部，用于 CRC
    QByteArray entries(entriesRegion, Qt::Uninitialized);
    if (f.read(entries.data(), entriesRegion) != entriesRegion)
    {
        qWarning() << "读取条目头部失败";
        return false;
    }

    // 8) CRC32 校验: 全局头 (排除 HeaderChecksum 字段自行) + 条目区域
    //    参与校验的全局头字节为 [0..15] (Magic+Version+HeaderSize+FileCount) + [20..23] (Reserved)
    const char *g = globalHdr;
    QByteArray crcData;
    crcData.reserve(16 + 4 + entries.size());
    crcData.append(g, 16);        // Magic, Version, HeaderSize, FileCount
    crcData.append(g + 20, 4);    // Reserved
    crcData.append(entries);
    quint32 actualCrc = computeCrc32(crcData);
    if (actualCrc != storedCrc)
    {
        qWarning() << "头部校验失败（CRC32 不匹配）";
        return false;
    }

    outFileCount    = fileCount;
    outHeaderSize   = hdrSize;
    outContentStart = hdrSize;
    return true;
}

// ========== FileManager 实现 ==========

FileManager::FileManager(QObject *parent) : QObject(parent)
{
}

// ---- 校验文件是否为有效归档 ----

bool FileManager::validateMergedFile(QString sFilePath)
{
    QFile f(sFilePath);
    if (!f.open(QIODevice::ReadOnly))
    {
        qWarning() << "打开文件失败:" << sFilePath;
        return false;
    }
    quint32 fc, hs, cs;
    bool ok = readAndValidateHeader(f, fc, hs, cs);
    f.close();
    return ok;
}

// ---- 合并文件 ----

void FileManager::QMergeFiles(QVector<QString> vtInputFiles, QString sOutputFile)
{
    m_bCancelRequested = false;

    int nCount = vtInputFiles.size();
    if (nCount <= 0)
    {
        emit operationFinished(false, tr("没有要合并的文件"));
        return;
    }

    QFile fOut(sOutputFile);
    if (!fOut.open(QIODevice::WriteOnly))
    {
        qWarning() << "打开输出文件失败:" << sOutputFile;
        emit operationFinished(false, tr("打开输出文件失败"));
        return;
    }

    // 预读文件信息
    struct FileInfo
    {
        QString path;
        qint64 size;
        qint64 contentPos;
    };
    QVector<FileInfo> fileVec;
    fileVec.reserve(nCount);

    // 头部 = 全局(24) + 条目(nCount * (20 + pathLen))
    qint64 contentStart = FWDAT_GLOBAL_HDR;
    for (int i = 0; i < nCount; i++)
    {
        QString fp = vtInputFiles.at(i);
        QFile inputFile(fp);
        if (!inputFile.open(QIODevice::ReadOnly))
        {
            qWarning() << "打开输入文件失败:" << fp;
            fOut.close();
            QFile::remove(sOutputFile);  // 清理残留的半成品文件
            emit operationFinished(false, tr("打开输入文件失败: %1").arg(fp));
            return;
        }
        FileInfo fi;
        fi.path = fp;
        fi.size = inputFile.size();
        inputFile.close();
        contentStart += FWDAT_ENTRY_FIXED + fi.path.toUtf8().size();
        fileVec.append(fi);
    }
    qint64 totalHdrSize = contentStart;

    // 头部不应超过 512 MB，否则 quint32 截断会导致 CRC 不匹配
    if (totalHdrSize < FWDAT_GLOBAL_HDR || totalHdrSize > 512LL * 1024 * 1024
        || quint64(totalHdrSize) > quint64(0xFFFFFFFFULL))
    {
        qWarning() << "合并文件头部过大:" << totalHdrSize;
        fOut.close();
        QFile::remove(sOutputFile);
        emit operationFinished(false, tr("合并文件头部过大"));
        return;
    }

    qint64 contentPos = totalHdrSize;
    for (int i = 0; i < nCount; i++)
    {
        fileVec[i].contentPos = contentPos;
        contentPos += fileVec[i].size;
    }

    // 计算 CRC（全局头不含 checksum 字段 + 全部条目数据）
    QByteArray crcData;
    // 全局头不含 checksum = 20 字节 (Magic 4 + Ver 4 + HdrSize 4 + FileCount 4 + Reserved 4)
    crcData.reserve(quint32(totalHdrSize));
    crcData.append(FWDAT_MAGIC, 4);            // Magic
    appendU32(crcData, FWDAT_VERSION);         // Version
    appendU32(crcData, quint32(totalHdrSize));  // HeaderSize
    appendU32(crcData, quint32(nCount));        // FileCount
    crcData.append(4, '\0');                    // Reserved (0)

    // 条目
    for (int i = 0; i < nCount; i++)
    {
        QByteArray pathBytes = fileVec[i].path.toUtf8();
        appendU32(crcData, quint32(pathBytes.size()));
        appendU64(crcData, quint64(fileVec[i].size));
        appendU64(crcData, quint64(fileVec[i].contentPos));
        crcData.append(pathBytes);
    }

    quint32 headerChecksum = computeCrc32(crcData);

    // —— 写全局头部 ——
    if (!fOut.seek(0))
    {
        qWarning() << "定位输出文件失败";
        fOut.close();
        emit operationFinished(false, tr("定位输出文件失败"));
        return;
    }
    fOut.write(FWDAT_MAGIC, 4);
    writeU32(fOut, FWDAT_VERSION);
    writeU32(fOut, quint32(totalHdrSize));
    writeU32(fOut, quint32(nCount));
    writeU32(fOut, headerChecksum);
    writeU32(fOut, 0); // Reserved

    // —— 写条目 ——
    for (int i = 0; i < nCount; i++)
    {
        QByteArray pathBytes = fileVec[i].path.toUtf8();
        writeU32(fOut, quint32(pathBytes.size()));
        writeU64(fOut, quint64(fileVec[i].size));
        writeU64(fOut, quint64(fileVec[i].contentPos));
        fOut.write(pathBytes);
    }

    // 总字节数用于进度条
    qint64 totalBytes = 0;
    for (int i = 0; i < nCount; i++)
        totalBytes += fileVec[i].size;
    qint64 copiedTotal = 0;
    int nLastPercent = -1;

    // —— 写文件内容 ——
    char *buf = new char[MAXBUFFERSIZE];
    bool bFailed = false;
    for (int i = 0; i < nCount; i++)
    {
        QFile fIn(fileVec[i].path);
        if (!fIn.open(QIODevice::ReadOnly))
        {
            qWarning() << "打开输入文件失败:" << fileVec[i].path;
            bFailed = true;
            break;
        }

        if (!fOut.seek(fileVec[i].contentPos))
        {
            qWarning() << "定位输出文件失败";
            fIn.close();
            bFailed = true;
            break;
        }

        qint64 copied = 0;
        while (copied < fileVec[i].size)
        {
            qint64 toRead = fileVec[i].size - copied;
            if (toRead > MAXBUFFERSIZE)
                toRead = MAXBUFFERSIZE;
            qint64 rd = fIn.read(buf, toRead);
            if (rd <= 0)
            {
                qWarning() << "读取输入文件失败:" << fileVec[i].path;
                bFailed = true;
                break;
            }
            if (fOut.write(buf, rd) != rd)
            {
                qWarning() << "写入输出文件失败";
                bFailed = true;
                break;
            }
            copied += rd;
            copiedTotal += rd;

            int nPercent = (totalBytes > 0) ? int(100 * copiedTotal / totalBytes) : 100;
            if (nPercent != nLastPercent)
            {
                nLastPercent = nPercent;
                QString sStatus = tr("正在合并: %1  (%2 / %3)")
                        .arg(getFileName(fileVec[i].path))
                        .arg(formatSize(copied))
                        .arg(formatSize(fileVec[i].size));
                emit progressChanged(copiedTotal, totalBytes, sStatus);
                QApplication::processEvents();
            }
            if (m_bCancelRequested)
            {
                qWarning() << "用户取消合并";
                bFailed = true;
                break;
            }
        }
        fIn.close();
        if (bFailed)
            break;
    }
    delete[] buf;
    fOut.close();

    // 任一文件写入失败 → 删除不完整的归档，避免生成"头部有效但内容缺失"的坏文件
    if (bFailed)
        QFile::remove(sOutputFile);

    QString sMsg;
    if (bFailed && m_bCancelRequested)
        sMsg = tr("操作已取消");
    else if (bFailed)
        sMsg = tr("文件合并失败");
    else
        sMsg = tr("文件合并成功");
    emit operationFinished(!bFailed, sMsg);
}

void FileManager::cancelOperation()
{
    m_bCancelRequested = true;
}

// ---- 分割文件 ----

void FileManager::QSplitFiles(QString sInputFile, bool bIsSaveAsOldPath, QString sSplitFileDir)
{
    QFile fIn(sInputFile);
    if (!fIn.open(QIODevice::ReadOnly))
    {
        qWarning() << "打开合并文件失败:" << sInputFile;
        return;
    }

    quint32 fileCount, hdrSize, contentStart;
    if (!readAndValidateHeader(fIn, fileCount, hdrSize, contentStart))
    {
        fIn.close();
        return;
    }

    // 回到条目起始位置
    if (!fIn.seek(FWDAT_GLOBAL_HDR))
    {
        qWarning() << "定位失败";
        fIn.close();
        return;
    }

    // 先读取全部条目元信息，再逐个提取内容
    struct SplitEntry
    {
        QString origPath;
        quint64 fileSize;
        quint64 contentPos;
    };
    QVector<SplitEntry> entries;
    entries.reserve(fileCount);

    for (int i = 0; i < fileCount; i++)
    {
        char plBuf[4];
        if (fIn.read(plBuf, 4) != 4)
        {
            qWarning() << "读取路径长度失败:" << i;
            break;
        }
        quint32 pathLen = readU32(plBuf);
        if (pathLen > 32768 || pathLen < 1)
        {
            qWarning() << "路径长度异常:" << pathLen;
            break;
        }

        char fsBuf[8];
        if (fIn.read(fsBuf, 8) != 8)
        {
            qWarning() << "读取文件大小失败:" << i;
            break;
        }
        quint64 fileSize = readU64(fsBuf);

        char cpBuf[8];
        if (fIn.read(cpBuf, 8) != 8)
        {
            qWarning() << "读取文件位置失败:" << i;
            break;
        }
        quint64 contentPos = readU64(cpBuf);

        QByteArray pathBytes(pathLen, Qt::Uninitialized);
        if (fIn.read(pathBytes.data(), pathLen) != pathLen)
        {
            qWarning() << "读取路径失败:" << i;
            break;
        }
        SplitEntry e;
        e.origPath = QString::fromUtf8(pathBytes);
        e.fileSize = fileSize;
        e.contentPos = contentPos;
        entries.append(e);
    }

    // 逐个提取文件内容
    char *buf = new char[MAXBUFFERSIZE];
    for (int i = 0; i < entries.size(); i++)
    {
        QString sOutputPath;
        if (bIsSaveAsOldPath)
            sOutputPath = entries[i].origPath;
        else
            sOutputPath = sSplitFileDir + "/" + getFileName(entries[i].origPath);

        // 确保输出目录存在，否则打开输出文件会失败
        QString sOutputDir = QFileInfo(sOutputPath).absolutePath();
        if (!sOutputDir.isEmpty() && !QDir().exists(sOutputDir))
        {
            if (!QDir().mkpath(sOutputDir))
            {
                qWarning() << "创建输出目录失败:" << sOutputDir;
                continue;
            }
        }

        // 跳至内容区
        if (!fIn.seek(entries[i].contentPos))
        {
            qWarning() << "定位文件内容失败:" << i;
            continue;
        }

        QFile fOut(sOutputPath);
        if (!fOut.open(QIODevice::WriteOnly))
        {
            qWarning() << "打开输出文件失败:" << sOutputPath;
            continue;
        }

        qint64 copied = 0;
        while (quint64(copied) < entries[i].fileSize)
        {
            quint64 remain = entries[i].fileSize - copied;
            qint64 toRead = remain < MAXBUFFERSIZE ? qint64(remain) : MAXBUFFERSIZE;
            qint64 rd = fIn.read(buf, toRead);
            if (rd <= 0)
            {
                qWarning() << "读取文件内容失败:" << sOutputPath;
                break;
            }
            if (fOut.write(buf, rd) != rd)
            {
                qWarning() << "写入输出文件失败:" << sOutputPath;
                break;
            }
            copied += rd;
            qDebug() << "split" << sOutputPath << copied << "/" << entries[i].fileSize;
        }
        fOut.close();
    }
    delete[] buf;
    fIn.close();
}

// ---- 加载合并文件（解析头部） ----

void FileManager::QLoadMergedFile(QString sFilePath)
{
    m_vtFileInfos.clear();
    m_mapFileInfos.clear();

    if (m_qFile.isOpen())
        m_qFile.close();

    m_qFile.setFileName(sFilePath);
    if (!m_qFile.open(QIODevice::ReadOnly))
    {
        qWarning() << "打开合并文件失败:" << sFilePath;
        return;
    }

    quint32 fileCount, hdrSize, contentStart;
    if (!readAndValidateHeader(m_qFile, fileCount, hdrSize, contentStart))
    {
        m_qFile.close();
        return;
    }

    // 回到条目起始
    if (!m_qFile.seek(FWDAT_GLOBAL_HDR))
    {
        qWarning() << "定位条目失败";
        m_qFile.close();
        return;
    }

    for (int i = 0; i < fileCount; i++)
    {
        // PathLength
        char plBuf[4];
        if (m_qFile.read(plBuf, 4) != 4)
        {
            qWarning() << "读取条目失败:" << i;
            m_vtFileInfos.clear();
            m_mapFileInfos.clear();
            m_qFile.close();
            return;
        }
        quint32 pathLen = readU32(plBuf);
        if (pathLen > 32768 || pathLen < 1)
        {
            qWarning() << "路径长度异常:" << pathLen;
            m_vtFileInfos.clear();
            m_mapFileInfos.clear();
            m_qFile.close();
            return;
        }

        // FileSize
        char fsBuf[8];
        if (m_qFile.read(fsBuf, 8) != 8)
        {
            qWarning() << "读取大小失败:" << i;
            m_vtFileInfos.clear();
            m_mapFileInfos.clear();
            m_qFile.close();
            return;
        }
        quint64 fileSize = readU64(fsBuf);

        // ContentPosition
        char cpBuf[8];
        if (m_qFile.read(cpBuf, 8) != 8)
        {
            qWarning() << "读取位置失败:" << i;
            m_vtFileInfos.clear();
            m_mapFileInfos.clear();
            m_qFile.close();
            return;
        }
        quint64 contentPos = readU64(cpBuf);

        // Path
        QByteArray pathBytes(pathLen, Qt::Uninitialized);
        if (m_qFile.read(pathBytes.data(), pathLen) != pathLen)
        {
            qWarning() << "读取路径失败:" << i;
            m_vtFileInfos.clear();
            m_mapFileInfos.clear();
            m_qFile.close();
            return;
        }
        QString sPath = QString::fromUtf8(pathBytes);

        FILEINFO fi;
        fi.sFilePath = sPath;
        fi.sFileName = getFileName(sPath);
        fi.nFileLen = fileSize;
        fi.nContentPositon = contentPos;
        fi.nState = 0;

        m_vtFileInfos.push_back(fi);
        m_mapFileInfos[sPath] = fi;
    }
}

void FileManager::unLoadMergedFile()
{
    if (m_qFile.isOpen())
        m_qFile.close();
    m_vtFileInfos.clear();
    m_mapFileInfos.clear();
}

bool FileManager::isArchiveLoaded() const
{
    return m_qFile.isOpen() && !m_qFile.fileName().isEmpty();
}

void FileManager::addFiles(const QVector<QString> &vtNewFiles)
{
    if (!isArchiveLoaded())
        return;

    bool bChanged = false;
    for (int i = 0; i < vtNewFiles.size(); ++i)
    {
        QString fp = vtNewFiles.at(i);
        QFileInfo fi(fp);
        if (!fi.exists() || !fi.isFile())
            continue;
        // 同一路径去重，避免重复写入
        if (m_mapFileInfos.contains(fp))
            continue;

        FILEINFO info;
        info.sFilePath = fp;
        info.sFileName = getFileName(fp);
        info.nFileLen = fi.size();
        info.nContentPositon = 0;   // 占位，重写归档后由 QLoadMergedFile 刷新
        info.nState = 0;
        m_vtFileInfos.append(info);
        m_mapFileInfos[fp] = info;
        m_setPendingFiles.insert(fp);
        bChanged = true;
    }

    // 仅修改内存，落盘动作交由 save()；没有任何实际新增则不置脏
    if (bChanged)
        m_bDirty = true;
}

bool FileManager::deleteFile(const QString &sFilePath)
{
    int idx = -1;
    for (int i = 0; i < m_vtFileInfos.size(); ++i)
    {
        if (m_vtFileInfos.at(i).sFilePath == sFilePath)
        {
            idx = i;
            break;
        }
    }
    if (idx < 0)
        return false;

    m_vtFileInfos.remove(idx);
    m_mapFileInfos.remove(sFilePath);
    m_setPendingFiles.remove(sFilePath);

    // 仅修改内存，落盘动作交由 save()
    m_bDirty = true;
    return true;
}

bool FileManager::isDirty() const
{
    return m_bDirty;
}

bool FileManager::isCancelRequested() const
{
    return m_bCancelRequested;
}

bool FileManager::save()
{
    if (!isArchiveLoaded())
        return false;

    // 没有待保存的改动
    if (!m_bDirty)
        return true;

    // 所有文件都被删除 -> 落盘时直接删除归档文件
    if (m_vtFileInfos.isEmpty())
    {
        QString sArchivePath = m_qFile.fileName();
        m_qFile.close();  // 必须先关闭句柄，Windows 才能删除该文件
        if (!QFile::remove(sArchivePath))
        {
            qWarning() << "删除空归档文件失败（可能被占用或只读）:" << sArchivePath;
            // 重新打开原文件，保留内存中的“已全部删除”状态与脏标记，便于下次重试
            m_qFile.setFileName(sArchivePath);
            m_qFile.open(QIODevice::ReadOnly);
            m_bDirty = true;
            return false;
        }
        m_vtFileInfos.clear();
        m_mapFileInfos.clear();
        m_setPendingFiles.clear();
        m_bDirty = false;
        return true;
    }

    // 重写整个归档并原子替换，成功后清除脏标记
    bool bOk = rewriteArchive();
    if (bOk)
        m_bDirty = false;
    return bOk;
}

bool FileManager::rewriteArchive()
{
    if (!isArchiveLoaded())
        return false;

    QString sArchivePath = m_qFile.fileName();
    int nCount = m_vtFileInfos.size();
    m_bCancelRequested = false;

    // 1) 仅需各条目大小即可计算头部与各内容偏移，无需读取内容
    qint64 contentStart = FWDAT_GLOBAL_HDR;
    for (int i = 0; i < nCount; ++i)
        contentStart += FWDAT_ENTRY_FIXED + m_vtFileInfos.at(i).sFilePath.toUtf8().size();
    qint64 totalHdrSize = contentStart;

    if (totalHdrSize < FWDAT_GLOBAL_HDR
        || totalHdrSize > 512LL * 1024 * 1024
        || quint64(totalHdrSize) > quint64(0xFFFFFFFFULL))
    {
        qWarning() << "归档头部过大:" << totalHdrSize;
        return false;
    }

    QVector<qint64> contentPosVec(nCount);
    qint64 cp = totalHdrSize;
    for (int i = 0; i < nCount; ++i)
    {
        contentPosVec[i] = cp;
        cp += m_vtFileInfos.at(i).nFileLen;
    }

    // 2) 写入临时文件：先写全局头 + 各条目元数据
    QString sTmpPath = sArchivePath + ".tmp.fwda";
    QFile::remove(sTmpPath);
    QFile fTmp(sTmpPath);
    if (!fTmp.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        qWarning() << "创建临时文件失败:" << sTmpPath;
        return false;
    }

    // 计算 CRC（与 QMergeFiles 完全一致：全局头不含 checksum + 全部条目）
    QByteArray crcData;
    crcData.reserve(quint32(totalHdrSize));
    crcData.append(FWDAT_MAGIC, 4);
    appendU32(crcData, FWDAT_VERSION);
    appendU32(crcData, quint32(totalHdrSize));
    appendU32(crcData, quint32(nCount));
    crcData.append(4, '\0'); // Reserved
    for (int i = 0; i < nCount; ++i)
    {
        QByteArray pathBytes = m_vtFileInfos.at(i).sFilePath.toUtf8();
        appendU32(crcData, quint32(pathBytes.size()));
        appendU64(crcData, quint64(m_vtFileInfos.at(i).nFileLen));
        appendU64(crcData, quint64(contentPosVec.at(i)));
        crcData.append(pathBytes);
    }
    quint32 headerChecksum = computeCrc32(crcData);

    // 写全局头部
    fTmp.write(FWDAT_MAGIC, 4);
    writeU32(fTmp, FWDAT_VERSION);
    writeU32(fTmp, quint32(totalHdrSize));
    writeU32(fTmp, quint32(nCount));
    writeU32(fTmp, headerChecksum);
    writeU32(fTmp, 0); // Reserved

    // 写条目
    for (int i = 0; i < nCount; ++i)
    {
        QByteArray pathBytes = m_vtFileInfos.at(i).sFilePath.toUtf8();
        writeU32(fTmp, quint32(pathBytes.size()));
        writeU64(fTmp, quint64(m_vtFileInfos.at(i).nFileLen));
        writeU64(fTmp, quint64(contentPosVec.at(i)));
        fTmp.write(pathBytes);
    }

    // 3) 流式写入内容：边从源（磁盘原文件 / 当前归档）读取、边写入临时文件，
    //    采用固定大小缓冲，避免一次性把全部文件内容读入内存（支持超大文件）。
    //    注意：流式写入失败分支在 m_qFile 仍打开时返回，原归档句柄与内存状态均保持不变。
    const qint64 kChunk = 1024 * 1024;  // 1MB 读写缓冲
    QByteArray buf;
    buf.resize(int(kChunk));

    // 总字节数用于进度条
    qint64 totalBytes = 0;
    for (int i = 0; i < nCount; ++i)
        totalBytes += m_vtFileInfos.at(i).nFileLen;
    qint64 copiedTotal = 0;
    int nLastPercent = -1;

    for (int i = 0; i < nCount; ++i)
    {
        const FILEINFO &fi = m_vtFileInfos.at(i);
        qint64 nRemaining = fi.nFileLen;

        if (m_setPendingFiles.contains(fi.sFilePath))
        {
            // 新加入的文件：内容来自磁盘原文件
            QFile src(fi.sFilePath);
            if (!src.open(QIODevice::ReadOnly))
            {
                qWarning() << "读取新增文件失败:" << fi.sFilePath;
                fTmp.close();
                QFile::remove(sTmpPath);
                return false;
            }
            while (nRemaining > 0)
            {
                qint64 toRead = qMin(nRemaining, kChunk);
                qint64 nGot = src.read(buf.data(), toRead);
                if (nGot <= 0)
                {
                    qWarning() << "读取新增文件内容不完整:" << fi.sFilePath;
                    src.close();
                    fTmp.close();
                    QFile::remove(sTmpPath);
                    return false;
                }
                if (fTmp.write(buf.data(), nGot) != nGot)
                {
                    qWarning() << "写入内容失败";
                    src.close();
                    fTmp.close();
                    QFile::remove(sTmpPath);
                    return false;
                }
                nRemaining -= nGot;
                copiedTotal += nGot;

                int nPercent = (totalBytes > 0) ? int(100 * copiedTotal / totalBytes) : 100;
                if (nPercent != nLastPercent)
                {
                    nLastPercent = nPercent;
                    QString sStatus = tr("正在保存: %1  (%2 / %3)")
                            .arg(getFileName(fi.sFilePath))
                            .arg(formatSize(copiedTotal))
                            .arg(formatSize(totalBytes));
                    emit progressChanged(copiedTotal, totalBytes, sStatus);
                    QApplication::processEvents();
                }
                if (m_bCancelRequested)
                {
                    qWarning() << "用户取消保存";
                    src.close();
                    fTmp.close();
                    QFile::remove(sTmpPath);
                    return false;
                }
            }
            src.close();
        }
        else
        {
            // 已存在于归档中的文件：内容来自当前 m_qFile
            if (!m_qFile.seek(fi.nContentPositon))
            {
                qWarning() << "定位内容失败:" << fi.sFilePath;
                fTmp.close();
                QFile::remove(sTmpPath);
                return false;
            }
            while (nRemaining > 0)
            {
                qint64 toRead = qMin(nRemaining, kChunk);
                qint64 nGot = m_qFile.read(buf.data(), toRead);
                if (nGot <= 0)
                {
                    qWarning() << "读取内容不完整:" << fi.sFilePath;
                    fTmp.close();
                    QFile::remove(sTmpPath);
                    return false;
                }
                if (fTmp.write(buf.data(), nGot) != nGot)
                {
                    qWarning() << "写入内容失败";
                    fTmp.close();
                    QFile::remove(sTmpPath);
                    return false;
                }
                nRemaining -= nGot;
                copiedTotal += nGot;

                int nPercent = (totalBytes > 0) ? int(100 * copiedTotal / totalBytes) : 100;
                if (nPercent != nLastPercent)
                {
                    nLastPercent = nPercent;
                    QString sStatus = tr("正在保存: %1  (%2 / %3)")
                            .arg(getFileName(fi.sFilePath))
                            .arg(formatSize(copiedTotal))
                            .arg(formatSize(totalBytes));
                    emit progressChanged(copiedTotal, totalBytes, sStatus);
                    QApplication::processEvents();
                }
                if (m_bCancelRequested)
                {
                    qWarning() << "用户取消保存";
                    // 注意：此处不关闭 m_qFile，以保留原归档句柄与内存状态（与流式失败分支一致）
                    fTmp.close();
                    QFile::remove(sTmpPath);
                    return false;
                }
            }
        }
    }
    fTmp.close();

    // 关键：此时所有内容已写入临时文件，必须关闭对原归档的句柄，
    // 否则在 Windows 下 QFile::rename 会因共享冲突（文件被本进程打开且无 FILE_SHARE_DELETE）而失败。
    m_qFile.close();

    // 4) 校验临时文件，再原子替换原文件（先备份，失败可还原）
    if (!validateMergedFile(sTmpPath))
    {
        qWarning() << "临时归档校验失败，取消替换";
        QFile::remove(sTmpPath);
        QLoadMergedFile(sArchivePath);  // 重新打开原归档，恢复为已加载状态
        return false;
    }

    QString sBak = sArchivePath + ".bak.fwda";
    QFile::remove(sBak);
    if (!QFile::rename(sArchivePath, sBak))
    {
        qWarning() << "备份原文件失败，取消替换";
        QFile::remove(sTmpPath);
        QLoadMergedFile(sArchivePath);  // 重新打开原归档，恢复为已加载状态
        return false;
    }
    if (!QFile::rename(sTmpPath, sArchivePath))
    {
        qWarning() << "替换原文件失败，尝试还原";
        QFile::rename(sBak, sArchivePath);
        QLoadMergedFile(sArchivePath);  // 重新打开原归档，恢复为已加载状态
        return false;
    }
    QFile::remove(sBak);

    // 5) 重新以只读方式加载，刷新内存中的条目与偏移
    QLoadMergedFile(sArchivePath);
    m_setPendingFiles.clear();
    return true;
}

void FileManager::QGetFileContent(QString sFilePath, char **szBuf, qint64 &nFileLen)
{
    *szBuf = nullptr;
    nFileLen = 0;

    QMap<QString, FILEINFO>::const_iterator cIt = m_mapFileInfos.find(sFilePath);
    if (cIt == m_mapFileInfos.end())
    {
        qDebug() << "未找到文件:" << sFilePath;
        return;
    }

    nFileLen = cIt.value().nFileLen;
    if (nFileLen <= 0)
        return;

    // 尚未落盘的待添加文件，内容直接来自磁盘原文件
    if (m_setPendingFiles.contains(sFilePath))
    {
        QFile src(sFilePath);
        if (!src.open(QIODevice::ReadOnly))
        {
            qWarning() << "读取新增文件失败:" << sFilePath;
            nFileLen = 0;
            return;
        }
        *szBuf = new char[nFileLen];
        qint64 nRead = src.read(*szBuf, nFileLen);
        src.close();
        if (nRead != nFileLen)
        {
            qWarning() << "读取新增文件内容不完整:" << sFilePath;
            delete[] *szBuf;
            *szBuf = nullptr;
            nFileLen = 0;
            return;
        }
        return;
    }

    if (!m_qFile.seek(cIt.value().nContentPositon))
    {
        qWarning() << "定位文件内容失败:" << sFilePath;
        nFileLen = 0;
        return;
    }

    *szBuf = new char[nFileLen];
    qint64 nRead = m_qFile.read(*szBuf, nFileLen);
    if (nRead != nFileLen)
    {
        qWarning() << "读取文件内容不完整:" << sFilePath << "期望:" << nFileLen << "实际:" << nRead;
        delete[] *szBuf;
        *szBuf = nullptr;
        nFileLen = 0;
        return;
    }
}

QVector<FILEINFO> FileManager::getFileInfos()
{
    return m_vtFileInfos;
}

void FileManager::outputFile(QString sFilePath, QString sOutputDir)
{
    QString sOutputFilePath;
    if (!sOutputDir.isEmpty())
    {
        // 确保输出目录存在
        if (!QDir().exists(sOutputDir) && !QDir().mkpath(sOutputDir))
            qWarning() << "创建输出目录失败:" << sOutputDir;
        sOutputFilePath = sOutputDir + "/" + getFileName(sFilePath);
    }
    else
        sOutputFilePath = sFilePath;

    char *szBuf = nullptr;
    qint64 nFileLen = 0;
    QGetFileContent(sFilePath, &szBuf, nFileLen);

    if (szBuf == nullptr)
    {
        qDebug() << "导出失败，未获取到文件内容:" << sFilePath;
        return;
    }

    QFile fTemp(sOutputFilePath);
    if (!fTemp.open(QIODevice::WriteOnly))
    {
        qWarning() << "打开输出文件失败:" << sOutputFilePath;
        delete[] szBuf;
        return;
    }
    if (fTemp.write(szBuf, nFileLen) != nFileLen)
        qWarning() << "写入输出文件失败:" << sOutputFilePath;
    fTemp.close();
    delete[] szBuf;
}

QString FileManager::getFileName(QString sFilePath)
{
    // 取最后一个 '/' 或 '\' 之后的部分作为文件名；
    // 正确同时处理 Unix(/)、Windows(\) 以及混合分隔符路径。
    int nPos = qMax(sFilePath.lastIndexOf('/'), sFilePath.lastIndexOf('\\'));
    return sFilePath.right(sFilePath.length() - (nPos + 1));
}

#include "filemanager.h"
#include <QApplication>
#include <QDebug>
#include <QByteArray>
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
    int nCount = vtInputFiles.size();
    if (nCount <= 0)
        return;

    QFile fOut(sOutputFile);
    if (!fOut.open(QIODevice::WriteOnly))
    {
        qWarning() << "打开输出文件失败:" << sOutputFile;
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

    // 头部 = 全局(24) + 条目(nCount * (16 + pathLen))
    qint64 contentStart = FWDAT_GLOBAL_HDR;
    for (int i = 0; i < nCount; i++)
    {
        QString fp = vtInputFiles.at(i);
        QFile tmp(fp);
        if (!tmp.open(QIODevice::ReadOnly))
        {
            qWarning() << "打开输入文件失败:" << fp;
            fOut.close();
            return;
        }
        FileInfo fi;
        fi.path = fp;
        fi.size = tmp.size();
        tmp.close();
        contentStart += FWDAT_ENTRY_FIXED + fi.path.toUtf8().size();
        fileVec.append(fi);
    }
    qint64 totalHdrSize = contentStart;
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

    // —— 写文件内容 ——
    char *buf = new char[MAXBUFFERSIZE];
    for (int i = 0; i < nCount; i++)
    {
        QFile fIn(fileVec[i].path);
        if (!fIn.open(QIODevice::ReadOnly))
        {
            qWarning() << "打开输入文件失败:" << fileVec[i].path;
            delete[] buf;
            fOut.close();
            return;
        }

        if (!fOut.seek(fileVec[i].contentPos))
        {
            qWarning() << "定位输出文件失败";
            fIn.close();
            delete[] buf;
            fOut.close();
            return;
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
                break;
            }
            if (fOut.write(buf, rd) != rd)
            {
                qWarning() << "写入输出文件失败";
                break;
            }
            copied += rd;
            qDebug() << "merge" << fileVec[i].path << copied << "/" << fileVec[i].size;
        }
        fIn.close();
    }
    delete[] buf;
    fOut.close();
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

    quint32 fileCount, hdrSize;
    quint32 tmp; // contentStart unused here, but needed for function
    if (!readAndValidateHeader(m_qFile, fileCount, hdrSize, tmp))
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
        sOutputFilePath = sOutputDir + "/" + getFileName(sFilePath);
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
    int pos = (sFilePath.lastIndexOf('\\') + 1) == 0
        ? sFilePath.lastIndexOf('/') + 1
        : sFilePath.lastIndexOf('\\') + 1;
    return sFilePath.right(sFilePath.length() - pos);
}

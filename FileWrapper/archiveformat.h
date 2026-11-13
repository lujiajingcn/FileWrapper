#ifndef ARCHIVEFORMAT_H
#define ARCHIVEFORMAT_H

// ============================================================
// FileWrapper 归档格式公共定义（v1 兼容读取 / v2 写入）
//
// 全局头 24 字节（v1 / v2 完全相同，小端）:
//   [0..3]   Magic "FWDA"
//   [4..7]   Version  uint32  1 = 旧格式, 2 = 当前格式
//   [8..11]  HeaderSize uint32 全局头 + 头部元数据区 + 条目头的总大小
//   [12..15] FileCount  uint32
//   [16..19] HeaderChecksum uint32  CRC32
//   [20..23] Reserved   uint32 (0)
//
// v1 头部区（HeaderSize - 24 字节）:
//   重复 FileCount 次:
//     PathLength u32 / FileSize u64 / ContentPos u64 / Path u8[PathLength]
//
// v2 头部区:
//   [24..27] RootLength u32
//   [28..]   Root u8[RootLength] (UTF-8, 合并时所有输入的公共父目录, 仅供显示)
//   重复 FileCount 次:
//     PathLength u32 / FileSize u64 / ContentPos u64 / ContentCrc32 u32 / Path u8[PathLength]
//   v2 的 Path 是相对 Root 的**相对路径**；v1 的 Path 是原始路径（可能是绝对路径）。
//   v2 快速删除采用「墓碑」：把该条目路径区的首字节改为 NUL，PathLength 与
//   槽位大小保持不变（后续条目才能继续按原 PathLength 对齐），解析时整条跳过。
//   头部 CRC 随之重算；内容区留待下一次完整重写时回收。
//   FileCount 记录物理槽位数（含墓碑），因此可能大于实际可见条目数。
//
// 校验说明:
//   HeaderChecksum 覆盖 全局头[0..15] + 全局头[20..23] + 整个头部区。
//   v2 每条的 ContentCrc32 覆盖该条目的内容区，用于检测内容篡改（P1-7）。
//   v1 没有内容校验字段，读取时跳过内容校验，保持对 1.dat 等旧归档的兼容。
// ============================================================

#include <QtGlobal>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QVector>
#include <QPair>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStorageInfo>

namespace FwArc {

static const char MAGIC[4] = {'F', 'W', 'D', 'A'};
static const quint32 VERSION_V1       = 1;
static const quint32 VERSION_V2       = 2;
static const quint32 CURRENT_VERSION  = VERSION_V2;
static const quint32 GLOBAL_HDR       = 24;
static const quint32 ENTRY_FIXED_V1   = 20;
static const quint32 ENTRY_FIXED_V2   = 24;
static const quint32 MAX_FILES        = 1000000;
static const quint32 MAX_PATH_LEN     = 32768;
static const quint32 MAX_ROOT_LEN     = 32768;
static const quint64 MAX_HDR_SIZE     = 512ULL * 1024 * 1024;
static const quint64 MAX_CONTENT_LEN  = (1ULL << 62);

struct Entry
{
    QString displayPath;
    QString relPath;
    quint64 size          = 0;
    quint64 contentPos    = 0;
    quint32 contentCrc    = 0;
    bool    hasContentCrc = false;
};

struct Meta
{
    quint32 version    = 0;
    quint32 headerSize = 0;
    quint32 fileCount  = 0;
    quint64 fileSize   = 0;
    QString root;
    QVector<Entry> entries;
};

inline quint32 readU32(const char *b)
{
    return quint32(quint8(b[0])) | quint32(quint8(b[1])) << 8
         | quint32(quint8(b[2])) << 16 | quint32(quint8(b[3])) << 24;
}

inline quint64 readU64(const char *b)
{
    quint64 v = 0;
    for (int i = 0; i < 8; ++i)
        v |= quint64(quint8(b[i])) << (i * 8);
    return v;
}

inline void appendU32(QByteArray &a, quint32 v)
{
    char b[4] = { char(v & 0xFF), char((v >> 8) & 0xFF),
                  char((v >> 16) & 0xFF), char((v >> 24) & 0xFF) };
    a.append(b, 4);
}

inline void appendU64(QByteArray &a, quint64 v)
{
    char b[8];
    for (int i = 0; i < 8; ++i) b[i] = char((v >> (i * 8)) & 0xFF);
    a.append(b, 8);
}

inline bool writeU32(QFile &f, quint32 v)
{
    char b[4] = { char(v & 0xFF), char((v >> 8) & 0xFF),
                  char((v >> 16) & 0xFF), char((v >> 24) & 0xFF) };
    return f.write(b, 4) == 4;
}

inline bool writeU64(QFile &f, quint64 v)
{
    char b[8];
    for (int i = 0; i < 8; ++i) b[i] = char((v >> (i * 8)) & 0xFF);
    return f.write(b, 8) == 8;
}

// CRC32（支持流式增量，避免一次分配完整头部）
class Crc32
{
public:
    Crc32() { reset(); }
    void reset() { m_crc = 0xFFFFFFFFu; }
    void update(const char *data, qint64 len)
    {
        const quint32 *table = crcTable();
        for (qint64 i = 0; i < len; ++i)
            m_crc = table[(m_crc ^ quint8(data[i])) & 0xFFu] ^ (m_crc >> 8);
    }
    void update(const QByteArray &data) { update(data.constData(), data.size()); }
    quint32 value() const { return ~m_crc; }
    static quint32 of(const QByteArray &data) { Crc32 c; c.update(data); return c.value(); }
private:
    // 函数局部静态：头文件被多个翻译单元包含时也只有一个表实例，无需单独 .cpp 定义
    static const quint32 *crcTable()
    {
        static quint32 table[256];
        static bool ready = false;
        if (!ready) {
            for (quint32 i = 0; i < 256; ++i) {
                quint32 c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                table[i] = c;
            }
            ready = true;
        }
        return table;
    }
    quint32 m_crc;
};

inline quint32 crc32Of(const QByteArray &data) { return Crc32::of(data); }

inline QString normalizePath(const QString &p)
{
    QString s = p;
    s.replace('\\', '/');
    while (s.endsWith('/') && s.size() > 1 && s.at(s.size() - 2) != ':')
        s.chop(1);
    return s;
}

inline bool isAbsolutePath(const QString &p)
{
    const QString s = normalizePath(p);
    if (s.startsWith('/')) return true;
    if (s.size() >= 2 && s.at(1) == ':' && s.at(0).isLetter()) return true;
    return false;
}

// 严格 UTF-8 合法性校验
inline bool isValidUtf8(const QByteArray &data)
{
    const unsigned char *p = reinterpret_cast<const unsigned char *>(data.constData());
    const qsizetype n = data.size();
    qsizetype i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        int extra = 0;
        if (c < 0x80) { ++i; continue; }
        else if ((c & 0xE0) == 0xC0) { extra = 1; if (c < 0xC2) return false; }
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) { extra = 3; if (c > 0xF4) return false; }
        else return false;
        if (i + extra >= n) return false;
        for (int k = 1; k <= extra; ++k)
            if ((p[i + k] & 0xC0) != 0x80) return false;
        i += extra + 1;
    }
    return true;
}

inline bool isReservedDeviceName(const QString &baseName)
{
    QString b = baseName;
    const int dot = b.indexOf('.');
    if (dot >= 0) b = b.left(dot);
    b = b.trimmed().toUpper();
    static const QStringList names = { "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" };
    return names.contains(b);
}

// 相对路径安全性检查：拒绝绝对路径、.. 回溯、非法字符、控制字符、设备名
inline bool isSafeRelativePath(const QString &rel, QString *err = nullptr)
{
    auto fail = [err](const QString &m) { if (err) *err = m; return false; };
    if (rel.isEmpty()) return fail(QStringLiteral("路径为空"));
    if (rel.size() > int(MAX_PATH_LEN)) return fail(QStringLiteral("路径过长"));
    if (isAbsolutePath(rel)) return fail(QStringLiteral("不允许绝对路径"));
    if (rel.contains(QChar(0))) return fail(QStringLiteral("路径含 NUL 字符"));

    const QString s = normalizePath(rel);
    const QStringList segs = s.split('/', Qt::SkipEmptyParts);
    if (segs.isEmpty()) return fail(QStringLiteral("路径为空"));
    static const QString illegal = QStringLiteral("<>:\"|?*\\");
    for (const QString &seg : segs) {
        if (seg == "." || seg == "..") return fail(QStringLiteral("路径含 '.' 或 '..' 段"));
        for (int i = 0; i < illegal.size(); ++i)
            if (seg.contains(illegal.at(i))) return fail(QStringLiteral("路径含非法字符"));
        for (int i = 0; i < seg.size(); ++i)
            if (seg.at(i).unicode() < 32) return fail(QStringLiteral("路径含控制字符"));
    }
    if (isReservedDeviceName(segs.last())) return fail(QStringLiteral("路径使用系统保留设备名"));
    return true;
}

// 把任意路径改造成安全的相对路径
inline QString sanitizeRelativePath(QString rel)
{
    rel = normalizePath(rel);
    if (rel.size() >= 2 && rel.at(1) == ':' && rel.at(0).isLetter()) rel = rel.mid(2);
    while (rel.startsWith('/')) rel.remove(0, 1);

    QStringList segs = rel.split('/', Qt::SkipEmptyParts);
    static const QString illegal = QStringLiteral("<>:\"|?*\\");
    for (QString &seg : segs) {
        if (seg == "." || seg == "..") seg = QStringLiteral("_");
        for (int i = 0; i < illegal.size(); ++i) seg.replace(illegal.at(i), '_');
        for (int i = 0; i < seg.size(); ++i)
            if (seg.at(i).unicode() < 32) seg[i] = '_';
    }
    if (segs.isEmpty()) segs << QStringLiteral("unnamed.bin");
    if (isReservedDeviceName(segs.last())) segs.last().prepend('_');
    return segs.join('/');
}

// 计算一组绝对路径的公共父目录（不含文件名段）
inline QString commonRoot(const QStringList &absPaths)
{
    QStringList common;
    bool first = true;
    for (const QString &p : absPaths) {
        const QStringList segs = normalizePath(p).split('/', Qt::SkipEmptyParts);
        const int dirCount = qMax(0, segs.size() - 1);
        if (first) {
            for (int i = 0; i < dirCount; ++i) common << segs.at(i);
            first = false;
        } else {
            int keep = 0;
            const int nMax = qMin(common.size(), dirCount);
            while (keep < nMax && common.at(keep) == segs.at(keep)) ++keep;
            while (common.size() > keep) common.removeLast();
        }
        if (common.isEmpty()) break;
    }
    return common.join('/');
}

inline QString makeRelative(const QString &absPath, const QString &root)
{
    const QString a = normalizePath(absPath);
    const QString r = normalizePath(root);
    if (!r.isEmpty() && a.startsWith(r + "/", Qt::CaseInsensitive))
        return a.mid(r.size() + 1);
    if (!r.isEmpty() && a.compare(r, Qt::CaseInsensitive) == 0)
        return QFileInfo(a).fileName();
    return QFileInfo(a).fileName();
}

inline QString displayPathFor(const QString &root, const QString &rel)
{
    if (root.isEmpty()) return rel;
    return normalizePath(root) + "/" + normalizePath(rel);
}

inline qint64 availableBytes(const QString &anyPath)
{
    QStorageInfo si(QFileInfo(anyPath).absolutePath());
    if (!si.isValid() || si.isReadOnly()) return -1;
    return si.bytesAvailable();
}

inline QString formatSize(qint64 bytes)
{
    const qint64 kb = 1024, mb = 1024 * 1024, gb = 1024LL * 1024 * 1024;
    if (bytes >= gb) return QString::number(double(bytes) / gb, 'f', 2) + " GB";
    if (bytes >= mb) return QString::number(double(bytes) / mb, 'f', 2) + " MB";
    if (bytes >= kb) return QString::number(double(bytes) / kb, 'f', 2) + " KB";
    return QString::number(bytes) + " B";
}

} // namespace FwArc

#endif // ARCHIVEFORMAT_H

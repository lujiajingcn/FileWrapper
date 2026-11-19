#ifndef ARCHIVEFORMAT_H
#define ARCHIVEFORMAT_H

// ============================================================
// FileWrapper 归档格式公共定义（v1/v2 兼容读取 / v3 写入）
//
// 全局头 24 字节（v1 / v2 / v3 完全相同，小端）:
//   [0..3]   Magic "FWDA"
//   [4..7]   Version  uint32  1 = 旧格式, 2 = 增强格式, 3 = 当前格式
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
// v3 头部区（当前写入格式，v1/v2 仍可读取）:
//   [24..27] RootLength u32
//   [28..]   Root u8[RootLength] (UTF-8)
//   重复 FileCount 次:
//     PathLength u32 / FileSize u64 / ContentPos u64 / ContentCrc32 u32
//     CreatedMs u64 / ModifiedMs u64 / AccessedMs u64 / Attributes u32
//     Path u8[PathLength]
//   三个时间都是 UTC 毫秒（1970-01-01 起），0 表示「未知/未记录」；
//   Attributes 存 FILE_ATTRIBUTE_* 的 R/H/S/A 位（见 filemetadata.h），
//   用于导出/分割时把源文件的只读/隐藏/系统/存档属性写回磁盘。
//   v3 不再使用墓碑（原地改头部）；删除与新增统一走完整重写。
//
// 校验说明:
//   HeaderChecksum 覆盖 全局头[0..15] + 全局头[20..23] + 整个头部区。
//   v2/v3 每条的 ContentCrc32 覆盖该条目的内容区，用于检测内容篡改（P1-7）。
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
#include <QCoreApplication>   // fileDialogFilter() 用 translate（命名空间里没有 tr()）

namespace FwArc {

// 归档的专属后缀：与文件头魔数 "FWDA" 同名，避免 .dat 这种通配后缀带来的混淆。
// 识别归档始终靠魔数（见 parseFromDevice），后缀只影响对话框过滤与人的判断。
static const char *const FILE_SUFFIX = ".fwda";
static const char *const LEGACY_SUFFIX = ".dat";   // v2 之前/早期版本用的后缀

// 「加载 / 另存为 / 拆分」三个对话框共用的文件过滤器。
// 新后缀在前，旧的 .dat 继续列出：老归档不用改名也仍然能在对话框里看到，
// 否则会出现「另存成了 .fwda，但加载框只认 *.dat，找不到文件」。
inline QString fileDialogFilter()
{
    return QCoreApplication::translate(
        "FwArc", "FileWrapper 归档 (*.fwda *.dat);;All Files (*.*)");
}

// 文件名是否看起来是归档（只看后缀，用于界面提示；
// 真正判定归档用的是文件头魔数）
inline bool hasArchiveSuffix(const QString &sPath)
{
    const QString sLower = sPath.toLower();
    return sLower.endsWith(QLatin1String(FILE_SUFFIX))
           || sLower.endsWith(QLatin1String(LEGACY_SUFFIX));
}

static const char MAGIC[4] = {'F', 'W', 'D', 'A'};
static const quint32 VERSION_V1       = 1;
static const quint32 VERSION_V2       = 2;
static const quint32 VERSION_V3       = 3;
static const quint32 CURRENT_VERSION  = VERSION_V3;
static const quint32 GLOBAL_HDR       = 24;
static const quint32 ENTRY_FIXED_V1   = 20;
static const quint32 ENTRY_FIXED_V2   = 24;
// v3：v2 的固定字段 + 3 个 u64 时间 + 1 个 u32 属性
static const quint32 ENTRY_FIXED_V3   = 52;
static const quint32 MAX_FILES        = 1000000;
static const quint32 MAX_PATH_LEN     = 32768;
static const quint32 MAX_ROOT_LEN     = 32768;
static const quint64 MAX_HDR_SIZE     = 512ULL * 1024 * 1024;
static const quint64 MAX_CONTENT_LEN  = (1ULL << 62);
// 时间戳合理上界（UTC 毫秒）：9999-12-31 23:59:59.999。
// 超出视为未知，避免伪造的 u64 在 QDateTime 转换/显示时变成无意义的值。
static const quint64 MAX_TIMESTAMP_MS = 253402300799999ULL;

// 条目固定字段长度（按版本）。v1/v2 没有文件系统元数据。
inline quint32 entryFixedSize(quint32 version)
{
    switch (version)
    {
    case VERSION_V1: return ENTRY_FIXED_V1;
    case VERSION_V2: return ENTRY_FIXED_V2;
    case VERSION_V3: return ENTRY_FIXED_V3;
    default:         return 0;
    }
}

// v2 起每条都有内容 CRC；v3 起每条都有文件系统元数据。
inline bool versionHasContentCrc(quint32 version) { return version >= VERSION_V2; }
inline bool versionHasFsMetadata(quint32 version) { return version >= VERSION_V3; }

// 把任意时间值收敛到「可显示」的范围：0 或 [1, MAX_TIMESTAMP_MS]。
inline quint64 sanitizeTimestamp(quint64 ms)
{
    return ms <= MAX_TIMESTAMP_MS ? ms : 0;
}

struct Entry
{
    QString displayPath;
    QString relPath;
    quint64 size          = 0;
    quint64 contentPos    = 0;
    quint32 contentCrc    = 0;
    bool    hasContentCrc = false;
    // v3 文件系统元数据（v1/v2 条目 hasFsMetadata=false、其余为 0）
    bool    hasFsMetadata = false;
    quint64 createdMs     = 0;
    quint64 modifiedMs    = 0;
    quint64 accessedMs    = 0;
    quint32 attributes    = 0;
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

// 严格 UTF-8 合法性校验（P2-2）
//
// 只做「字节序列是否合法 UTF-8」的判断，不做替换。必须完整拒绝：
//   * 三/四字节 overlong（E0 80..9F、F0 80..8F）；
//   * UTF-16 代理区（ED A0..BF）与大于 U+10FFFF 的码点（F4 90..BF 及以上）；
//   * 截断序列与 5/6 字节历史形式。
// 旧实现只检查「后续字节高两位是 10」，会把上述非法序列当成合法 UTF-8，
// 随后被 Qt 替换成 U+FFFD，造成名称混淆（归档路径、文本预览共用同一判据）。
inline bool isValidUtf8(const QByteArray &data)
{
    const unsigned char *p = reinterpret_cast<const unsigned char *>(data.constData());
    const qsizetype n = data.size();
    qsizetype i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        if (c < 0x80) { ++i; continue; }

        int extra = 0;
        unsigned char lo = 0x80, hi = 0xBF;   // 第二字节允许范围
        if ((c & 0xE0) == 0xC0)      { extra = 1; if (c < 0xC2) return false; }
        else if ((c & 0xF0) == 0xE0) { extra = 2;
            if (c == 0xE0) lo = 0xA0;          // 拒绝 overlong
            if (c == 0xED) hi = 0x9F; }        // 拒绝 UTF-16 代理
        else if ((c & 0xF8) == 0xF0) { extra = 3;
            if (c > 0xF4) return false;        // > U+10FFFF
            if (c == 0xF0) lo = 0x90;          // 拒绝 overlong
            if (c == 0xF4) hi = 0x8F; }        // 到 U+10FFFF 为止
        else return false;                     // 0xF8..0xFF / 孤立的续字节

        if (i + extra >= n) return false;      // 截断
        if (p[i + 1] < lo || p[i + 1] > hi) return false;
        for (int k = 2; k <= extra; ++k)
            if ((p[i + k] & 0xC0) != 0x80) return false;
        i += extra + 1;
    }
    return true;
}

// Windows 保留设备名（P1-11）：按「去掉尾部空格/点、截到第一个点」后的基名判断。
// 旧实现只用在最后一段，且不处理 "CON." / "CON " / "COM1.txt" 之后的变体，
// 结果是中间目录叫 CON 或段名带尾随点/空格时仍能通过校验，写出后无法再创建。
inline bool isReservedDeviceName(const QString &baseName)
{
    QString b = baseName;
    while (b.endsWith(QLatin1Char('.')) || b.endsWith(QLatin1Char(' ')))
        b.chop(1);
    const int dot = b.indexOf(QLatin1Char('.'));
    if (dot >= 0) b = b.left(dot);
    b = b.trimmed().toUpper();
    if (b.isEmpty()) return false;
    static const QStringList names = { "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" };
    return names.contains(b);
}

// 单个路径段是否可作为 Windows 文件名（P1-11）。
// 归档内的路径段最终都会落到磁盘，必须在写归档/导出/分割之前统一收紧：
//   * 拒绝 "." / ".."、空段、控制字符、< > : " | ? * \、NUL；
//   * 拒绝尾随点/空格（资源管理器会静默改名，后续按原路径已找不到）；
//   * 拒绝任何一段使用保留设备名（含 "CON."、"COM1 " 这类变体）；
//   * 拒绝超过 255 个 UTF-16 单元的段（NTFS 单段上限）。
inline bool isSafePathSegment(const QString &seg, QString *err = nullptr)
{
    auto fail = [err](const QString &m) { if (err) *err = m; return false; };
    if (seg.isEmpty()) return fail(QStringLiteral("路径含空段"));
    if (seg == "." || seg == "..") return fail(QStringLiteral("路径含 '.' 或 '..' 段"));
    if (seg.size() > 255) return fail(QStringLiteral("路径段过长: %1").arg(seg.left(40)));
    if (seg.contains(QChar(0))) return fail(QStringLiteral("路径含 NUL 字符"));
    static const QString illegal = QStringLiteral("<>:\"|?*\\");
    for (int i = 0; i < seg.size(); ++i) {
        const QChar ch = seg.at(i);
        if (ch.unicode() < 32) return fail(QStringLiteral("路径含控制字符"));
        if (illegal.contains(ch)) return fail(QStringLiteral("路径含非法字符"));
    }
    if (seg.endsWith(QLatin1Char('.')) || seg.endsWith(QLatin1Char(' ')))
        return fail(QStringLiteral("路径段不能以点或空格结尾: %1").arg(seg.left(40)));
    if (isReservedDeviceName(seg))
        return fail(QStringLiteral("路径使用系统保留设备名: %1").arg(seg));
    return true;
}

// 相对路径安全性检查：逐段走 isSafePathSegment，拒绝绝对路径与回溯
inline bool isSafeRelativePath(const QString &rel, QString *err = nullptr)
{
    auto fail = [err](const QString &m) { if (err) *err = m; return false; };
    if (rel.isEmpty()) return fail(QStringLiteral("路径为空"));
    if (rel.size() > int(MAX_PATH_LEN)) return fail(QStringLiteral("路径过长"));
    if (isAbsolutePath(rel)) return fail(QStringLiteral("不允许绝对路径"));
    if (rel.contains(QChar(0))) return fail(QStringLiteral("路径含 NUL 字符"));

    const QString s = normalizePath(rel);
    const QStringList segs = s.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (segs.isEmpty()) return fail(QStringLiteral("路径为空"));
    for (const QString &seg : segs) {
        QString why;
        if (!isSafePathSegment(seg, &why)) return fail(why);
    }
    return true;
}

// ============================================================
// 统一的 checked 尺寸运算（P1-11）
//
// 归档头部里的大小/偏移都是 quint64，而 Qt 容器与 API 使用 qint64/qsizetype。
// 直接累加或强制转换在 32 位构建、或伪造的超大长度下会回绕，进而出现
// 「小分配 + 大读取」或负偏移。所有跨类型运算集中走这里。
// ============================================================
inline bool checkedAdd64(qint64 a, qint64 b, qint64 *pOut)
{
    if (pOut == nullptr) return false;
    if (b > 0 && a > (std::numeric_limits<qint64>::max)() - b) return false;
    if (b < 0 && a < (std::numeric_limits<qint64>::min)() - b) return false;
    *pOut = a + b;
    return true;
}

inline bool checkedAddU64(quint64 a, quint64 b, quint64 *pOut)
{
    if (pOut == nullptr) return false;
    if (a > (std::numeric_limits<quint64>::max)() - b) return false;
    *pOut = a + b;
    return true;
}

// 能否安全地从 quint64 长度转换成 qsizetype（内存块/QByteArray 尺寸）。
inline bool fitsInQsizetype(quint64 v)
{
    return v <= quint64((std::numeric_limits<qsizetype>::max)());
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

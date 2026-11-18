#include "zipreader.h"

#include <zlib.h>
#include <cstring>
#include <limits>

// ---------- 小端读取 ----------
static inline quint32 rd32(const char *b)
{
    const uchar *u = reinterpret_cast<const uchar *>(b);
    return quint32(u[0]) | (quint32(u[1]) << 8)
         | (quint32(u[2]) << 16) | (quint32(u[3]) << 24);
}
static inline quint16 rd16(const char *b)
{
    const uchar *u = reinterpret_cast<const uchar *>(b);
    return quint16(quint32(u[0]) | (quint32(u[1]) << 8));
}

// ---------- 路径归一化 ----------
QString ZipReader::normalize(const QString &p)
{
    QString r = p;
    r.replace('\\', '/');
    while (r.startsWith("./")) r.remove(0, 2);
    while (r.startsWith('/'))  r.remove(0, 1);
    while (r.endsWith('/'))    r.chop(1);
    return r;
}

// ---------- 打开并解析中央目录 ----------
bool ZipReader::open(const QByteArray &data)
{
    m_data = data;          // 保留原始字节，entry() 据此读取本地文件头与压缩数据
    m_entries.clear();
    m_ok = false;

    if (data.size() < 22)
        return false;

    // 1) 自尾部向前定位 EndOfCentralDirectory（签名 0x06054b50）
    qint64 eocd = -1;
    qint64 start = data.size() - 22;
    if (start < 0) start = 0;
    // ZIP 规范：EOCD 只可能出现在最后 22+65535 字节内，不必全文件反向扫描。
    const qint64 lowest = qMax<qint64>(0, data.size() - 22 - 65535);
    for (qint64 i = start; i >= lowest; --i)
    {
        if (rd32(data.constData() + i) == 0x06054b50u) { eocd = i; break; }
    }
    if (eocd < 0)
        return false;

    const char *e = data.constData() + eocd;
    quint32 cdOff   = rd32(e + 16);
    quint16 cdCount = rd16(e + 10);
    if (quint64(cdOff) + 4 > quint64(data.size()))
        return false;

    // 2) 遍历中央目录条目（签名 0x02014b50）
    qint64 p = cdOff;
    for (int idx = 0; idx < cdCount; ++idx)
    {
        if (p + 46 > data.size()) return false;
        if (rd32(data.constData() + p) != 0x02014b50u) return false;

        quint16 flags     = rd16(data.constData() + p + 8);   // general purpose flag
        quint16 method    = rd16(data.constData() + p + 10);
        quint32 compSize  = rd32(data.constData() + p + 20);
        quint32 uncompSize= rd32(data.constData() + p + 24);
        quint16 nameLen   = rd16(data.constData() + p + 28);
        quint16 extraLen  = rd16(data.constData() + p + 30);
        quint16 commLen   = rd16(data.constData() + p + 32);
        quint32 localOff  = rd32(data.constData() + p + 42);

        if (p + 46 + nameLen > data.size()) return false;
        QString name = QString::fromUtf8(data.constData() + p + 46, nameLen);

        // 加密条目（general purpose flag bit0）无法解压：不收录，
        // 否则会把密文当正文/图片解析，甚至喂给后续 XML 解析。
        if (!(flags & 0x1))
        {
            Entry en;
            en.name      = normalize(name);
            en.method    = method;
            en.compSize  = compSize;
            en.uncompSize= uncompSize;
            en.localOff  = localOff;
            m_entries.append(en);
        }

        p += 46 + nameLen + extraLen + commLen;
    }

    m_ok = true;
    return true;
}

QStringList ZipReader::entryNames() const
{
    QStringList l;
    for (const Entry &e : m_entries)
        l.append(e.name);
    return l;
}

bool ZipReader::contains(const QString &name) const
{
    QString n = normalize(name);
    for (const Entry &e : m_entries)
        if (e.name.compare(n, Qt::CaseInsensitive) == 0)
            return true;
    return false;
}

QByteArray ZipReader::entry(const QString &name) const
{
    QString n = normalize(name);
    for (const Entry &e : m_entries)
    {
        if (e.name.compare(n, Qt::CaseInsensitive) != 0)
            continue;

        // 读本地文件头，取真实的文件名/扩展字段长度以跳到数据区
        if (e.localOff + 30 > m_data.size())
            return QByteArray();
        if (rd32(m_data.constData() + e.localOff) != 0x04034b50u)
            return QByteArray();   // 本地头签名不匹配，拒绝按伪造偏移解读
        quint16 lnameLen  = rd16(m_data.constData() + e.localOff + 26);
        quint16 lextraLen = rd16(m_data.constData() + e.localOff + 28);
        qint64 dataOff = e.localOff + 30 + lnameLen + lextraLen;
        if (dataOff + e.compSize > m_data.size())
            return QByteArray();

        // compSize 已被上面的边界检查限制在 m_data 之内；用 Qt6 的 qsizetype
        // 构造 QByteArray，避免 quint32 -> int 截断成负数或丢失高位。
        QByteArray comp(m_data.constData() + dataOff,
                        static_cast<qsizetype>(e.compSize));

        if (e.method == 0)        return comp;          // stored
        if (e.method == 8)        return inflate(comp, e.uncompSize);  // deflate
        return QByteArray();                        // 其它压缩方式不支持
    }
    return QByteArray();
}

// ---------- zlib 裸 deflate 解压 ----------
QByteArray ZipReader::inflate(const QByteArray &comp, qint64 uncompSize) const
{
    // ZIP 头中的 uncompSize 是攻击者可控的 quint32：原实现用 int() 转换会在
    // >2 GiB 时变负，且按声明值一次分配可被 ZIP bomb 吃掉数 GB 内存。
    constexpr qint64 kMaxEntryBytes = 64 * 1024 * 1024;   // 单条目解压上限 64 MiB
    if (uncompSize < 0 || uncompSize > kMaxEntryBytes)
        return QByteArray();
    // zlib 的 avail_in / avail_out 是 uInt（32 位），超过会被静默截断
    if (comp.size() > qint64(std::numeric_limits<uInt>::max()))
        return QByteArray();

    const qsizetype maxCap = static_cast<qsizetype>(kMaxEntryBytes);
    qsizetype cap = uncompSize > 0 ? static_cast<qsizetype>(uncompSize)
                                   : qMax<qsizetype>(16384, comp.size() * 2);
    if (cap > maxCap) cap = maxCap;

    for (int attempt = 0; attempt < 6; ++attempt)
    {
        QByteArray out(cap, Qt::Uninitialized);
        z_stream strm;
        std::memset(&strm, 0, sizeof(strm));
        strm.next_in   = (Bytef *)comp.constData();
        strm.avail_in  = (uInt)comp.size();
        strm.next_out  = (Bytef *)out.data();
        strm.avail_out = (uInt)out.size();

        // -MAX_WBITS：裸 deflate（无 zlib 头），这正是 ZIP 条目的压缩格式
        // 注意用 :: 前缀调用全局 zlib 函数，避免与本类的 inflate() 成员名冲突
        if (::inflateInit2(&strm, -MAX_WBITS) != Z_OK)
            return QByteArray();

        int r = ::inflate(&strm, Z_FINISH);
        if (r == Z_STREAM_END)
        {
            out.resize(static_cast<qsizetype>(strm.total_out));
            ::inflateEnd(&strm);
            return out;
        }
        ::inflateEnd(&strm);

        if (r != Z_BUF_ERROR)
            return QByteArray();   // 真正的损坏，放弃
        cap = qMin(cap * 2, maxCap);   // 缓冲区不足，放大重试（有上限）
    }
    return QByteArray();
}

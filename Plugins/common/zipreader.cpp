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

namespace {
constexpr quint32 kSigEocd = 0x06054b50u;
constexpr quint32 kSigCentral = 0x02014b50u;
constexpr quint32 kSigLocal = 0x04034b50u;
constexpr quint32 kSigZip64EocdLocator = 0x07064b50u;
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
    m_lastError.clear();
    m_budget.totalBytes = 0;
    m_budget.extractions = 0;
    m_budget.exhausted = false;

    if (data.size() < 22)
    {
        m_lastError = QStringLiteral("文件过小，不是有效的 ZIP");
        return false;
    }

    // 1) 自尾部向前定位 EndOfCentralDirectory（签名 0x06054b50）
    //
    // ZIP 注释里可以出现伪造的 EOCD 签名，所以候选必须通过完整性校验：
    //   * 注释长度声明与「记录结束位置」一致（允许尾部少量附加数据）；
    //   * 单磁盘字段为 0，磁盘条目数与总条目数一致；
    //   * 中央目录偏移/大小落在文件内。
    // 不满足就继续向前找，而不是拿第一个命中的假签名当目录。
    qint64 eocd = -1;
    const qint64 start = qMax<qint64>(0, data.size() - 22);
    const qint64 lowest = qMax<qint64>(0, data.size() - 22 - 65535);
    for (qint64 i = start; i >= lowest; --i)
    {
        if (rd32(data.constData() + i) != kSigEocd)
            continue;

        if (i + 22 > data.size())
            continue;

        const char *e = data.constData() + i;
        const quint16 commentLen = rd16(e + 20);
        if (i + 22 + qint64(commentLen) > data.size())
            continue;                       // 注释长度越界：伪签名
        const quint16 diskNo     = rd16(e + 4);
        const quint16 cdDisk     = rd16(e + 6);
        const quint16 diskCount  = rd16(e + 8);
        const quint16 cdCount    = rd16(e + 10);
        const quint32 cdSize     = rd32(e + 12);
        const quint32 cdOff      = rd32(e + 16);

        if (diskNo != 0 || cdDisk != 0 || diskCount != cdCount)
        {
            // 分卷 / ZIP64 不可用：给调用方一个明确原因，而不是笼统失败
            m_lastError = (cdCount == 0xFFFFu || cdOff == 0xFFFFFFFFu
                           || cdSize == 0xFFFFFFFFu || diskNo == 0xFFFFu)
                              ? QStringLiteral("不支持 ZIP64 归档")
                              : QStringLiteral("不支持分卷 ZIP 归档");
            return false;
        }
        if (quint64(cdOff) + quint64(cdSize) > quint64(data.size()))
            continue;

        eocd = i;
        break;
    }
    if (eocd < 0)
    {
        m_lastError = QStringLiteral("未找到有效的中央目录结束记录（EOCD）");
        return false;
    }

    const char *e = data.constData() + eocd;
    const quint32 cdOff   = rd32(e + 16);
    const quint32 cdCount = rd16(e + 10);
    const quint32 cdSize  = rd32(e + 12);

    // ZIP64 定位器出现在 EOCD 之前时明确拒绝（本读取器不支持 ZIP64）
    if (eocd >= 20 && rd32(data.constData() + eocd - 20) == kSigZip64EocdLocator)
    {
        m_lastError = QStringLiteral("不支持 ZIP64 归档");
        return false;
    }
    if (cdCount > quint32(m_budget.maxEntries))
    {
        m_lastError = QStringLiteral("ZIP 条目数超过上限（%1）").arg(m_budget.maxEntries);
        return false;
    }

    // 2) 遍历中央目录条目（签名 0x02014b50）
    qint64 p = qint64(cdOff);
    const qint64 cdEnd = qint64(cdOff) + qint64(cdSize);
    for (quint32 idx = 0; idx < cdCount; ++idx)
    {
        if (p + 46 > data.size() || p + 46 > cdEnd)
        {
            m_lastError = QStringLiteral("中央目录条目越界");
            return false;
        }
        if (rd32(data.constData() + p) != kSigCentral)
        {
            m_lastError = QStringLiteral("中央目录签名不匹配");
            return false;
        }

        const quint16 flags     = rd16(data.constData() + p + 8);   // general purpose flag
        const quint16 method    = rd16(data.constData() + p + 10);
        const quint32 crc32     = rd32(data.constData() + p + 16);
        const quint32 compSize  = rd32(data.constData() + p + 20);
        const quint32 uncompSize= rd32(data.constData() + p + 24);
        const quint16 nameLen   = rd16(data.constData() + p + 28);
        const quint16 extraLen  = rd16(data.constData() + p + 30);
        const quint16 commLen   = rd16(data.constData() + p + 32);
        const quint32 localOff  = rd32(data.constData() + p + 42);

        if (p + 46 + qint64(nameLen) > data.size())
        {
            m_lastError = QStringLiteral("中央目录条目名越界");
            return false;
        }
        const QString name = QString::fromUtf8(data.constData() + p + 46, nameLen);

        // 加密条目（general purpose flag bit0）无法解压：不收录，
        // 否则会把密文当正文/图片解析，甚至喂给后续 XML 解析。
        if (!(flags & 0x1))
        {
            Entry en;
            en.name       = normalize(name);
            en.method     = method;
            en.compSize   = compSize;
            en.uncompSize = uncompSize;
            en.localOff   = localOff;
            en.crc32      = crc32;
            m_entries.append(en);
        }

        p += 46 + qint64(nameLen) + qint64(extraLen) + qint64(commLen);
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
    const QString n = normalize(name);
    for (const Entry &e : m_entries)
        if (e.name.compare(n, Qt::CaseInsensitive) == 0)
            return true;
    return false;
}

// 统一预算入口（P1-12）：stored 与 deflate 都走这里。
bool ZipReader::chargeBudget(qint64 bytes, const QString &sName) const
{
    if (bytes < 0)
    {
        m_lastError = QStringLiteral("条目长度异常：%1").arg(sName);
        return false;
    }
    if (bytes > m_budget.maxEntryBytes)
    {
        m_lastError = QStringLiteral("单条目解压超过上限（%1 B）：%2")
                          .arg(m_budget.maxEntryBytes).arg(sName);
        m_budget.exhausted = true;
        return false;
    }
    if (m_budget.totalBytes + bytes > m_budget.maxTotalBytes)
    {
        m_lastError = QStringLiteral("全文档累计解压超过上限（%1 B）")
                          .arg(m_budget.maxTotalBytes);
        m_budget.exhausted = true;
        return false;
    }
    if (m_budget.extractions >= m_budget.maxExtractions)
    {
        m_lastError = QStringLiteral("解压次数超过上限（%1）").arg(m_budget.maxExtractions);
        m_budget.exhausted = true;
        return false;
    }
    m_budget.totalBytes += bytes;
    ++m_budget.extractions;
    return true;
}

QByteArray ZipReader::entry(const QString &name) const
{
    const QString n = normalize(name);
    for (const Entry &e : m_entries)
    {
        if (e.name.compare(n, Qt::CaseInsensitive) != 0)
            continue;

        // 读本地文件头，取真实的文件名/扩展字段长度以跳到数据区
        if (e.localOff < 0 || e.localOff + 30 > m_data.size())
        {
            m_lastError = QStringLiteral("本地文件头越界：%1").arg(n);
            return QByteArray();
        }
        if (rd32(m_data.constData() + e.localOff) != kSigLocal)
        {
            m_lastError = QStringLiteral("本地文件头签名不匹配：%1").arg(n);
            return QByteArray();   // 拒绝按伪造偏移解读
        }
        const quint16 lnameLen  = rd16(m_data.constData() + e.localOff + 26);
        const quint16 lextraLen = rd16(m_data.constData() + e.localOff + 28);
        const qint64 dataOff = e.localOff + 30 + qint64(lnameLen) + qint64(lextraLen);
        if (dataOff < 0 || e.compSize < 0 || dataOff + e.compSize > m_data.size())
        {
            m_lastError = QStringLiteral("压缩数据越界：%1").arg(n);
            return QByteArray();
        }

        // P1-12：stored 与 deflate 一样先过预算，再复制/解压。
        // stored 条目不压缩，若按声明长度直接复制，ZIP bomb 只需声明 4 GiB
        // 的 stored 数据就能让调用方一次性分配。
        if (!chargeBudget(e.uncompSize, n))
            return QByteArray();

        // 中央目录声明“解压后 0 字节”，却带有压缩数据：这是伪造/损坏的条目。
        // 旧实现只比较 `uncompSize > 0` 时的长度，导致这种条目可以解出最多
        // 64 MiB 内容而预算只记 0 字节；同一图片被 2048 次引用时即可反复解压，
        // 把累计预算和 CPU 上限完全绕过。空条目必须真的没有数据。
        if (e.uncompSize == 0)
        {
            if (e.compSize != 0)
            {
                m_lastError = QStringLiteral("条目长度声明异常（声明 0 字节但有压缩数据）：%1")
                                  .arg(n);
                return QByteArray();
            }
            return QByteArray();
        }

        if (e.method == 0)
        {
            if (e.compSize != e.uncompSize)
            {
                m_lastError = QStringLiteral("stored 条目长度不一致：%1").arg(n);
                return QByteArray();
            }
            QByteArray out(m_data.constData() + dataOff,
                           static_cast<qsizetype>(e.compSize));
            // P2-3：stored 也要核对 CRC，不能把损坏内容当正常数据返回
            const quint32 actual = quint32(::crc32(0L, reinterpret_cast<const Bytef *>(out.constData()),
                                                    uInt(out.size())));
            if (actual != e.crc32)
            {
                m_lastError = QStringLiteral("内容 CRC 不匹配：%1").arg(n);
                return QByteArray();
            }
            return out;
        }
        if (e.method == 8)
            return inflate(QByteArray(m_data.constData() + dataOff,
                                      static_cast<qsizetype>(e.compSize)), e);
        m_lastError = QStringLiteral("不支持的压缩方式 %1：%2").arg(e.method).arg(n);
        return QByteArray();                        // 其它压缩方式不支持
    }
    m_lastError = QStringLiteral("归档中没有该条目：%1").arg(n);
    return QByteArray();
}

// ---------- zlib 裸 deflate 解压 ----------
QByteArray ZipReader::inflate(const QByteArray &comp, const Entry &e) const
{
    // ZIP 头中的 uncompSize 是攻击者可控的 quint32：>2 GiB 时 int() 会变负，
    // 按声明值一次分配可被 ZIP bomb 吃掉数 GB 内存。预算已在 entry() 中收取。
    if (e.uncompSize <= 0 || e.uncompSize > m_budget.maxEntryBytes)
        return QByteArray();
    // zlib 的 avail_in / avail_out 是 uInt（32 位），超过会被静默截断
    if (comp.size() > qint64(std::numeric_limits<uInt>::max()))
    {
        m_lastError = QStringLiteral("压缩数据过大：%1").arg(e.name);
        return QByteArray();
    }

    // 声明长度就是本次解压的硬上限：只分配这么大的输出缓冲，一次 inflate。
    // 旧实现以 maxEntryBytes 为上限反复放大重试，遇到「声明 1 字节、实际 64 MiB」
    // 的条目会先完整解压多遍再判定长度不符；同一图片被大量引用时可造成 CPU 放大。
    const qsizetype cap = static_cast<qsizetype>(e.uncompSize);
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
    {
        m_lastError = QStringLiteral("无法初始化解压器：%1").arg(e.name);
        return QByteArray();
    }

    const int r = ::inflate(&strm, Z_FINISH);
    const qint64 nProduced = qint64(strm.total_out);
    ::inflateEnd(&strm);

    if (r == Z_BUF_ERROR || nProduced > qint64(cap))
    {
        // 输出比中央目录声明的长度更长：不能继续放大分配，直接拒绝。
        m_lastError = QStringLiteral("解压输出超过声明长度：%1").arg(e.name);
        return QByteArray();
    }
    if (r != Z_STREAM_END)
    {
        m_lastError = QStringLiteral("解压失败：%1").arg(e.name);
        return QByteArray();
    }

    out.resize(static_cast<qsizetype>(nProduced));
    // P2-3：实际输出长度与声明长度必须一致（声明值用于预算/分配，
    // 不能同时相信它；两者不一致说明头部或数据被篡改）。
    if (nProduced != qint64(e.uncompSize))
    {
        m_lastError = QStringLiteral("解压长度与声明不一致：%1").arg(e.name);
        return QByteArray();
    }
    const quint32 actual = quint32(::crc32(0L, reinterpret_cast<const Bytef *>(out.constData()),
                                            uInt(out.size())));
    if (actual != e.crc32)
    {
        m_lastError = QStringLiteral("内容 CRC 不匹配：%1").arg(e.name);
        return QByteArray();
    }
    return out;
}

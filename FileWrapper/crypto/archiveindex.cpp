#include "archiveindex.h"

namespace FwArcIndex {

namespace {

// 逐字节读取游标；越界时 ok=false。
struct Cursor
{
    const QByteArray &data;
    qint64 pos = 0;
    bool ok = true;

    explicit Cursor(const QByteArray &d) : data(d) {}

    const char *take(qint64 n)
    {
        if (!ok || n < 0 || pos + n > data.size()) { ok = false; return nullptr; }
        const char *p = data.constData() + pos;
        pos += n;
        return p;
    }
    quint32 u32() { const char *p = take(4); return p ? FwArc::readU32(p) : 0; }
    quint64 u64() { const char *p = take(8); return p ? FwArc::readU64(p) : 0; }
    QByteArray bytes(qint64 n)
    {
        const char *p = take(n);
        return p ? QByteArray(p, int(n)) : QByteArray();
    }
};

} // namespace

qint64 exactBytes(const QVector<FwArc::Entry> &entries, const QString &root,
                  QString *psErr)
{
    auto fail = [psErr](const QString &m) { if (psErr) *psErr = m; return qint64(-1); };
    const QByteArray rootBytes = root.toUtf8();
    if (rootBytes.size() > int(FwArc::MAX_ROOT_LEN))
        return fail(QStringLiteral("原始根过长"));

    qint64 n = 4 + rootBytes.size() + 4;
    for (int i = 0; i < entries.size(); ++i)
    {
        const FwArc::Entry &e = entries.at(i);
        const QByteArray path = e.relPath.toUtf8();
        if (path.isEmpty() || path.size() > int(FwArc::MAX_PATH_LEN))
            return fail(QStringLiteral("第 %1 条路径长度异常").arg(i));
        n += qint64(FwArc::ENTRY_FIXED) + path.size()
             + qint64(FwArc::tagPayloadBytes(e.tags));
        if (n > qint64(FwArc::MAX_HDR_SIZE))
            return fail(QStringLiteral("索引过大"));
    }
    return n;
}

bool build(const QVector<FwArc::Entry> &entries, const QString &root,
           qint64 capacity, QByteArray *pOut, QString *psErr)
{
    auto fail = [psErr](const QString &m) { if (psErr) *psErr = m; return false; };
    if (pOut == nullptr)
        return fail(QStringLiteral("内部错误：输出为空"));

    QString err;
    const qint64 exact = exactBytes(entries, root, &err);
    if (exact < 0)
        return fail(err);
    if (capacity < exact)
        return fail(QStringLiteral("索引容量不足（需要 %1，实际 %2）").arg(exact).arg(capacity));
    if (quint64(capacity) > FwArc::MAX_HDR_SIZE || quint64(capacity) > 0xFFFFFFFFULL)
        return fail(QStringLiteral("索引容量过大"));

    const QByteArray rootBytes = root.toUtf8();
    QByteArray out;
    out.reserve(int(capacity));
    FwArc::appendU32(out, quint32(rootBytes.size()));
    out.append(rootBytes);
    FwArc::appendU32(out, quint32(entries.size()));

    for (int i = 0; i < entries.size(); ++i)
    {
        const FwArc::Entry &e = entries.at(i);
        const QByteArray path = e.relPath.toUtf8();
        const QStringList tags = e.tags;

        FwArc::appendU64(out, e.entryId);
        FwArc::appendU64(out, e.size);
        FwArc::appendU64(out, e.contentPos);
        FwArc::appendU32(out, e.contentCrc);
        FwArc::appendU64(out, e.createdMs);
        FwArc::appendU64(out, e.modifiedMs);
        FwArc::appendU64(out, e.accessedMs);
        FwArc::appendU32(out, e.attributes);
        FwArc::appendU32(out, e.chunkCount);
        if (e.fileNonce.size() != 8)
            return fail(QStringLiteral("第 %1 条 FileNonce 必须为 8 字节").arg(i));
        out.append(e.fileNonce);
        FwArc::appendU32(out, quint32(tags.size()));
        FwArc::appendU32(out, quint32(path.size()));
        out.append(path);

        for (const QString &tag : tags)
        {
            const QByteArray tb = tag.toUtf8();
            if (tb.isEmpty() || tb.size() > int(FwArc::MAX_TAG_LEN))
                return fail(QStringLiteral("第 %1 条标记长度异常").arg(i));
            FwArc::appendU32(out, quint32(tb.size()));
            out.append(tb);
        }
    }

    if (out.size() > capacity)
        return fail(QStringLiteral("索引序列化超出容量"));
    out.append(QByteArray(int(capacity - out.size()), '\0'));
    *pOut = out;
    return true;
}

bool parse(const QByteArray &plain, FwArc::Meta *pMeta, QString *psErr)
{
    auto fail = [psErr](const QString &m) { if (psErr) *psErr = m; return false; };
    if (pMeta == nullptr || plain.isEmpty())
        return fail(QStringLiteral("索引为空"));

    Cursor c(plain);
    const quint32 rootLen = c.u32();
    if (!c.ok || rootLen > FwArc::MAX_ROOT_LEN)
        return fail(QStringLiteral("原始根长度异常: %1").arg(rootLen));
    const QByteArray rootBytes = c.bytes(qint64(rootLen));
    if (!c.ok)
        return fail(QStringLiteral("原始根越界"));
    if (!FwArc::isValidUtf8(rootBytes))
        return fail(QStringLiteral("原始根不是合法 UTF-8"));

    const quint32 count = c.u32();
    if (!c.ok || count == 0 || count > FwArc::MAX_FILES)
        return fail(QStringLiteral("文件数量异常: %1").arg(count));

    FwArc::Meta meta;
    meta.version    = FwArc::CURRENT_VERSION;
    meta.fileCount  = count;
    meta.root       = QString::fromUtf8(rootBytes);
    meta.entries.reserve(int(count));

    for (quint32 i = 0; i < count; ++i)
    {
        if (!c.ok || c.pos + qint64(FwArc::ENTRY_FIXED) > plain.size())
            return fail(QStringLiteral("第 %1 条记录越界").arg(i));

        FwArc::Entry e;
        e.entryId    = c.u64();
        e.size       = c.u64();
        e.contentPos = c.u64();
        e.contentCrc = c.u32();
        const quint64 created  = c.u64();
        const quint64 modified = c.u64();
        const quint64 accessed = c.u64();
        const quint32 attrs    = c.u32();
        e.chunkCount = c.u32();
        e.fileNonce  = c.bytes(8);
        const quint32 tagCount  = c.u32();
        const quint32 pathLen   = c.u32();
        if (!c.ok)
            return fail(QStringLiteral("第 %1 条固定字段读取失败").arg(i));

        if (pathLen == 0 || pathLen > FwArc::MAX_PATH_LEN)
            return fail(QStringLiteral("第 %1 条路径长度异常: %2").arg(i).arg(pathLen));
        if (tagCount > FwArc::MAX_TAGS)
            return fail(QStringLiteral("第 %1 条标记数量异常: %2").arg(i).arg(tagCount));

        const QByteArray path = c.bytes(qint64(pathLen));
        if (!c.ok)
            return fail(QStringLiteral("第 %1 条路径越界").arg(i));
        if (!FwArc::isValidUtf8(path))
            return fail(QStringLiteral("第 %1 条路径不是合法 UTF-8").arg(i));

        QString why;
        if (!FwArc::isSafeRelativePath(QString::fromUtf8(path), &why))
            return fail(QStringLiteral("第 %1 条路径不安全：%2").arg(i).arg(why));

        QStringList tags;
        tags.reserve(int(tagCount));
        for (quint32 t = 0; t < tagCount; ++t)
        {
            const quint32 tagLen = c.u32();
            if (!c.ok || tagLen == 0 || tagLen > FwArc::MAX_TAG_LEN)
                return fail(QStringLiteral("第 %1 条标记长度异常").arg(i));
            const QByteArray tb = c.bytes(tagLen);
            if (!c.ok)
                return fail(QStringLiteral("第 %1 条标记越界").arg(i));
            if (!FwArc::isValidUtf8(tb))
                return fail(QStringLiteral("第 %1 条标记不是合法 UTF-8").arg(i));
            const QString tag = QString::fromUtf8(tb);
            QString tagErr;
            if (!FwArc::isValidTag(tag, &tagErr))
                return fail(QStringLiteral("第 %1 条标记非法：%2").arg(i).arg(tagErr));
            tags << tag;
        }

        if (e.size > FwArc::MAX_CONTENT_LEN)
            return fail(QStringLiteral("第 %1 条文件大小异常: %2").arg(i).arg(e.size));
        if (e.chunkCount != FwArc::chunkCountFor(e.size))
            return fail(QStringLiteral("第 %1 条块数与大小不匹配").arg(i));
        if (e.entryId == 0)
            return fail(QStringLiteral("第 %1 条 EntryId 非法").arg(i));

        e.relPath     = QString::fromUtf8(path);
        e.displayPath = FwArc::displayPathFor(meta.root, e.relPath);
        e.createdMs   = FwArc::sanitizeTimestamp(created);
        e.modifiedMs  = FwArc::sanitizeTimestamp(modified);
        e.accessedMs  = FwArc::sanitizeTimestamp(accessed);
        e.attributes  = attrs;   // 调用方再用 FwMeta::ATTR_MASK 收紧
        e.tags        = tags;
        meta.entries.append(e);
    }

    // 尾部预留区必须是零：AEAD 已保证完整性，这里额外收紧，避免“把垃圾当预留”。
    while (c.pos < plain.size())
    {
        if (plain.at(int(c.pos)) != '\0')
            return fail(QStringLiteral("索引预留区非零"));
        ++c.pos;
    }

    *pMeta = meta;
    return true;
}

} // namespace FwArcIndex

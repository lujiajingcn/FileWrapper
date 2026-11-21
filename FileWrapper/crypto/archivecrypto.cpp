#include "archivecrypto.h"

#include "archiveformat.h"
#include "cngcipher.h"

#include <cstring>

namespace FwArcV5 {

namespace {

void appendU32BE(QByteArray &a, quint32 v)
{
    char b[4] = { char((v >> 24) & 0xFF), char((v >> 16) & 0xFF),
                  char((v >> 8) & 0xFF), char(v & 0xFF) };
    a.append(b, 4);
}

void appendU64BE(QByteArray &a, quint64 v)
{
    char b[8];
    for (int i = 0; i < 8; ++i)
        b[i] = char((v >> (56 - i * 8)) & 0xFF);
    a.append(b, 8);
}

// wrappedKey 的 AAD：只含整个归档生命周期内不变的字段。
QByteArray wrappedKeyAad(quint32 keyScheme, const QByteArray &uuid)
{
    QByteArray aad(FwArc::MAGIC, 4);
    QByteArray v;
    FwArc::appendU32(v, FwArc::CURRENT_VERSION);
    aad.append(v);
    v.clear();
    FwArc::appendU32(v, keyScheme);
    aad.append(v);
    aad.append(uuid);
    return aad;
}

// 内容块 AAD：40 字节。
QByteArray chunkAad(quint64 entryId, const QByteArray &uuid,
                    quint32 chunkIndex, quint32 chunkCount)
{
    QByteArray aad(FwArc::MAGIC, 4);
    QByteArray v;
    FwArc::appendU32(v, FwArc::CURRENT_VERSION);
    aad.append(v);
    aad.append(uuid);
    appendU64BE(aad, entryId);
    appendU32BE(aad, chunkIndex);
    appendU32BE(aad, chunkCount);
    return aad;
}

QByteArray chunkIv(const QByteArray &fileNonce, quint32 chunkIndex)
{
    QByteArray iv = fileNonce.left(8);
    appendU32BE(iv, chunkIndex);
    return iv;
}

} // namespace

bool newArchiveSecrets(QByteArray *cmk, QByteArray *uuid, QString *err)
{
    if (cmk == nullptr || uuid == nullptr)
        return false;
    QByteArray k(FwCrypto::kAesKeyLen, '\0');
    QByteArray u(FwArc::ARCHIVE_UUID_LEN, '\0');
    if (!FwCrypto::randomBytes(k.data(), k.size())
        || !FwCrypto::randomBytes(u.data(), u.size())) {
        if (err) *err = QStringLiteral("生成归档密钥失败（CSPRNG）");
        return false;
    }
    *cmk = k;
    *uuid = u;
    return true;
}

bool buildGlobalHeader(const GlobalHeader &h, QByteArray *out, QString *err)
{
    if (out == nullptr || h.uuid.size() != int(FwArc::ARCHIVE_UUID_LEN)
        || h.indexIv.size() != int(FwArc::INDEX_IV_LEN)) {
        if (err) *err = QStringLiteral("全局头参数非法");
        return false;
    }
    QByteArray b(FwArc::MAGIC, 4);
    FwArc::appendU32(b, FwArc::CURRENT_VERSION);
    FwArc::appendU32(b, h.flags);
    FwArc::appendU32(b, h.keyScheme);
    FwArc::appendU32(b, h.indexCipherLen);
    FwArc::appendU32(b, h.wrappedKeyLen);
    FwArc::appendU32(b, h.extLen);
    FwArc::appendU32(b, 0);            // Reserved
    b.append(h.uuid);
    b.append(h.indexIv);
    FwArc::appendU32(b, 0);            // Reserved2
    if (b.size() != int(FwArc::GLOBAL_HDR)) {
        if (err) *err = QStringLiteral("全局头长度异常");
        return false;
    }
    *out = b;
    return true;
}

bool parseGlobalHeader(const QByteArray &in, GlobalHeader *out, QString *err)
{
    auto fail = [err](const QString &m) { if (err) *err = m; return false; };
    if (out == nullptr || in.size() != int(FwArc::GLOBAL_HDR))
        return fail(QStringLiteral("全局头长度异常"));

    if (std::memcmp(in.constData(), FwArc::MAGIC, 4) != 0)
        return fail(QStringLiteral("魔数不匹配，不是有效的归档"));

    const quint32 version = FwArc::readU32(in.constData() + 4);
    if (version != FwArc::CURRENT_VERSION)
        return fail(QStringLiteral("不支持的归档版本: %1（当前只支持 %2）")
                        .arg(version).arg(FwArc::CURRENT_VERSION));

    const quint32 flags          = FwArc::readU32(in.constData() + 8);
    const quint32 keyScheme      = FwArc::readU32(in.constData() + 12);
    const quint32 indexCipherLen = FwArc::readU32(in.constData() + 16);
    const quint32 wrappedKeyLen  = FwArc::readU32(in.constData() + 20);
    const quint32 extLen         = FwArc::readU32(in.constData() + 24);
    const quint32 reserved       = FwArc::readU32(in.constData() + 28);
    const quint32 reserved2      = FwArc::readU32(in.constData() + 60);

    // 未知标志位/保留位一律拒绝：避免未来格式被旧程序“猜着读”。
    if ((flags & ~quint32(0x4)) != 0)
        return fail(QStringLiteral("归档标志位不受支持"));
    if (keyScheme != FwArc::KEY_SCHEME_ARGON2_AESGCM)
        return fail(QStringLiteral("不支持的密钥方案: %1").arg(keyScheme));
    if (wrappedKeyLen != FwArc::WRAPPED_KEY_LEN)
        return fail(QStringLiteral("wrappedKey 长度异常: %1").arg(wrappedKeyLen));
    if (indexCipherLen < FwArc::GCM_TAG_LEN || quint64(indexCipherLen) > FwArc::MAX_HDR_SIZE)
        return fail(QStringLiteral("索引长度异常: %1").arg(indexCipherLen));
    if (reserved != 0 || reserved2 != 0)
        return fail(QStringLiteral("归档头保留字段非零"));
    if (extLen > 0)
        return fail(QStringLiteral("不支持的扩展区（v5.0 应为空）"));

    out->flags          = flags;
    out->keyScheme      = keyScheme;
    out->indexCipherLen = indexCipherLen;
    out->wrappedKeyLen  = wrappedKeyLen;
    out->extLen         = extLen;
    out->uuid           = in.mid(32, int(FwArc::ARCHIVE_UUID_LEN));
    out->indexIv        = in.mid(48, int(FwArc::INDEX_IV_LEN));
    return true;
}

bool sealWrappedKey(const QByteArray &productKey, const QByteArray &cmk,
                    const QByteArray &uuid, QByteArray *out, QString *err)
{
    auto fail = [err](const QString &m) { if (err) *err = m; return false; };
    if (out == nullptr || productKey.size() != FwCrypto::kAesKeyLen
        || cmk.size() != FwCrypto::kAesKeyLen
        || uuid.size() != int(FwArc::ARCHIVE_UUID_LEN))
        return fail(QStringLiteral("wrappedKey 参数非法"));

    QByteArray iv(FwArc::WRAPPED_IV_LEN, '\0');
    if (!FwCrypto::randomBytes(iv.data(), iv.size()))
        return fail(QStringLiteral("生成 wrappedKey IV 失败"));

    const QByteArray aad = wrappedKeyAad(FwArc::KEY_SCHEME_ARGON2_AESGCM, uuid);
    QByteArray ct;
    if (!FwCrypto::aesGcmSeal(productKey, iv, aad, cmk, &ct))
        return fail(QStringLiteral("包裹 CMK 失败"));

    QByteArray blob = iv;
    blob.append(ct);
    if (blob.size() != int(FwArc::WRAPPED_KEY_LEN))
        return fail(QStringLiteral("wrappedKey 长度异常"));
    *out = blob;
    return true;
}

bool openWrappedKey(const QByteArray &productKey, const QByteArray &uuid,
                    const QByteArray &wrapped, QByteArray *cmk, QString *err)
{
    auto fail = [err](const QString &m) { if (err) *err = m; return false; };
    if (cmk == nullptr || productKey.size() != FwCrypto::kAesKeyLen
        || wrapped.size() != int(FwArc::WRAPPED_KEY_LEN)
        || uuid.size() != int(FwArc::ARCHIVE_UUID_LEN))
        return fail(QStringLiteral("wrappedKey 参数非法"));

    const QByteArray iv = wrapped.left(int(FwArc::WRAPPED_IV_LEN));
    const QByteArray ct = wrapped.mid(int(FwArc::WRAPPED_IV_LEN));
    const QByteArray aad = wrappedKeyAad(FwArc::KEY_SCHEME_ARGON2_AESGCM, uuid);
    if (!FwCrypto::aesGcmOpen(productKey, iv, aad, ct, cmk))
        return fail(QStringLiteral("密钥不匹配或归档已损坏"));
    return true;
}

QByteArray indexKey(const QByteArray &cmk, const QByteArray &uuid)
{
    QByteArray k;
    FwCrypto::hkdfSha256(cmk, uuid, QByteArrayLiteral("FWDA/v5/index"), 32, &k);
    return k;
}

QByteArray fileKey(const QByteArray &cmk, const QByteArray &fileNonce, quint64 entryId)
{
    QByteArray info = QByteArrayLiteral("FWDA/v5/file");
    appendU64BE(info, entryId);
    QByteArray k;
    FwCrypto::hkdfSha256(cmk, fileNonce, info, 32, &k);
    return k;
}

bool sealIndex(const QByteArray &cmk, const QByteArray &uuid, const QByteArray &indexIv,
               const QByteArray &plain, QByteArray *cipher, QString *err)
{
    if (cipher == nullptr || indexIv.size() != int(FwArc::INDEX_IV_LEN))
        return false;
    GlobalHeader h;
    h.uuid = uuid;
    h.indexIv = indexIv;
    h.indexCipherLen = quint32(plain.size()) + FwArc::GCM_TAG_LEN;
    QByteArray gh;
    if (!buildGlobalHeader(h, &gh, err))
        return false;

    const QByteArray k = indexKey(cmk, uuid);
    const bool ok = FwCrypto::aesGcmSeal(k, indexIv, gh, plain, cipher);
    if (!ok && err) *err = QStringLiteral("加密索引失败");
    return ok;
}

bool openIndex(const QByteArray &cmk, const QByteArray &uuid, const QByteArray &indexIv,
               const QByteArray &cipher, QByteArray *plain, QString *err)
{
    auto fail = [err](const QString &m) { if (err) *err = m; return false; };
    if (plain == nullptr || indexIv.size() != int(FwArc::INDEX_IV_LEN)
        || cipher.size() < int(FwArc::GCM_TAG_LEN))
        return fail(QStringLiteral("索引参数非法"));

    GlobalHeader h;
    h.uuid = uuid;
    h.indexIv = indexIv;
    h.indexCipherLen = quint32(cipher.size());
    QByteArray gh;
    if (!buildGlobalHeader(h, &gh, err))
        return false;

    const QByteArray k = indexKey(cmk, uuid);
    if (!FwCrypto::aesGcmOpen(k, indexIv, gh, cipher, plain))
        return fail(QStringLiteral("归档已损坏或被篡改（索引认证失败）"));
    return true;
}

bool sealChunk(const QByteArray &cmk, const QByteArray &uuid, quint64 entryId,
               const QByteArray &fileNonce, quint32 chunkIndex, quint32 chunkCount,
               const QByteArray &plain, QByteArray *out, QString *err)
{
    if (out == nullptr || fileNonce.size() != 8)
        return false;
    const QByteArray k = fileKey(cmk, fileNonce, entryId);
    const QByteArray aad = chunkAad(entryId, uuid, chunkIndex, chunkCount);
    const bool ok = FwCrypto::aesGcmSeal(k, chunkIv(fileNonce, chunkIndex), aad, plain, out);
    if (!ok && err) *err = QStringLiteral("加密内容块失败");
    return ok;
}

bool openChunk(const QByteArray &cmk, const QByteArray &uuid, quint64 entryId,
               const QByteArray &fileNonce, quint32 chunkIndex, quint32 chunkCount,
               const QByteArray &cipher, QByteArray *plain, QString *err)
{
    auto fail = [err](const QString &m) { if (err) *err = m; return false; };
    if (plain == nullptr || fileNonce.size() != 8
        || cipher.size() < int(FwArc::GCM_TAG_LEN))
        return fail(QStringLiteral("内容块参数非法"));
    const QByteArray k = fileKey(cmk, fileNonce, entryId);
    const QByteArray aad = chunkAad(entryId, uuid, chunkIndex, chunkCount);
    if (!FwCrypto::aesGcmOpen(k, chunkIv(fileNonce, chunkIndex), aad, cipher, plain))
        return fail(QStringLiteral("归档已损坏或被篡改（内容认证失败）"));
    return true;
}

QByteArray newFileNonce()
{
    QByteArray n(8, '\0');
    FwCrypto::randomBytes(n.data(), n.size());
    return n;
}

quint64 newEntryId()
{
    quint64 id = 0;
    FwCrypto::randomBytes(&id, int(sizeof(id)));
    return id == 0 ? 1 : id;   // 0 保留
}

} // namespace FwArcV5

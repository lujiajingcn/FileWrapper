#ifndef FWCRYPTO_ARCHIVECRYPTO_H
#define FWCRYPTO_ARCHIVECRYPTO_H

// ============================================================
// FWDA v5 归档加密层（格式相关，不依赖 Qt Widgets / FileManager）
//
// 负责：
//   * 64 字节全局头的构造/解析
//   * PRODUCT_KEY 包裹 CMK / 解包
//   * 索引密钥与文件密钥的 HKDF 派生
//   * 索引 AEAD 封装/解封
//   * 1 MiB 内容块的 AEAD 封装/解封
//
// 所有布局常量以 archiveformat.h 为唯一来源。
// ============================================================

#include <QByteArray>
#include <QString>

namespace FwArcV5 {

struct GlobalHeader
{
    quint32 flags          = 0;
    quint32 keyScheme      = 1;     // FwArc::KEY_SCHEME_ARGON2_AESGCM
    quint32 indexCipherLen = 0;
    quint32 wrappedKeyLen  = 60;    // FwArc::WRAPPED_KEY_LEN
    quint32 extLen         = 0;
    QByteArray uuid;                // 16 字节
    QByteArray indexIv;             // 12 字节
};

// 单个条目的加解密参数（读来源 / 写目标共用）。
struct EntryCrypto
{
    QByteArray cmk;
    QByteArray uuid;
    quint64    entryId    = 0;
    QByteArray fileNonce;           // 8 字节
    quint32    chunkCount = 0;
};

// 新归档需要的随机参数：CMK(32B) 与 ArchiveUuid(16B)。
bool newArchiveSecrets(QByteArray *cmk, QByteArray *uuid, QString *err = nullptr);

// 64 字节全局头。
bool buildGlobalHeader(const GlobalHeader &h, QByteArray *out, QString *err = nullptr);
bool parseGlobalHeader(const QByteArray &in64, GlobalHeader *out, QString *err = nullptr);

// wrappedKey（60 字节）：WkIv(12) || CMK 密文(32) || Tag(16)。
bool sealWrappedKey(const QByteArray &productKey, const QByteArray &cmk,
                    const QByteArray &uuid, QByteArray *out, QString *err = nullptr);
bool openWrappedKey(const QByteArray &productKey, const QByteArray &uuid,
                    const QByteArray &wrapped, QByteArray *cmk, QString *err = nullptr);

// 索引 / 文件密钥。
QByteArray indexKey(const QByteArray &cmk, const QByteArray &uuid);
QByteArray fileKey(const QByteArray &cmk, const QByteArray &fileNonce, quint64 entryId);

// 索引：明文（含预留补零）→ 密文（+16 tag）。
bool sealIndex(const QByteArray &cmk, const QByteArray &uuid, const QByteArray &indexIv,
               const QByteArray &plain, QByteArray *cipher, QString *err = nullptr);
bool openIndex(const QByteArray &cmk, const QByteArray &uuid, const QByteArray &indexIv,
               const QByteArray &cipher, QByteArray *plain, QString *err = nullptr);

// 内容块：blockIndex 从 0 开始；chunkCount 为该条目总块数。
bool sealChunk(const QByteArray &cmk, const QByteArray &uuid, quint64 entryId,
               const QByteArray &fileNonce, quint32 chunkIndex, quint32 chunkCount,
               const QByteArray &plain, QByteArray *out, QString *err = nullptr);
bool openChunk(const QByteArray &cmk, const QByteArray &uuid, quint64 entryId,
               const QByteArray &fileNonce, quint32 chunkIndex, quint32 chunkCount,
               const QByteArray &cipher, QByteArray *plain, QString *err = nullptr);

// 生成 8 字节 FileNonce；生成归档内唯一 EntryId。
QByteArray newFileNonce();
quint64 newEntryId();

} // namespace FwArcV5

#endif // FWCRYPTO_ARCHIVECRYPTO_H

// ============================================================
// 加密层单测：AES-256-GCM / HKDF-SHA256 / Argon2id 派生
//
//   * AES-256-GCM：NIST 空明文 KAT + 随机往返 + 篡改/AAD 不匹配检测
//   * HKDF-SHA256：RFC 5869 Test Case 1
//   * Argon2id：官方 KAT（t=3,m=32,p=4, secret/ad）自检
//   * deriveProductKey：确定性、空串拒绝、指纹长度
// ============================================================

#include "crypto/cngcipher.h"
#include "crypto/argonderive.h"
#include "crypto/archivecrypto.h"
#include "crypto/archiveindex.h"
#include "crypto/keyring.h"

#include <QByteArray>
#include <QElapsedTimer>

#include <cstdio>
#include <cstring>

namespace {

int g_fail = 0;

void check(bool ok, const char *psName)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", psName);
    if (!ok)
        ++g_fail;
}

QByteArray randomBytes(int n)
{
    QByteArray b(n, '\0');
    if (!FwCrypto::randomBytes(b.data(), n))
        b.clear();
    return b;
}

void testAesGcm()
{
    // NIST GCM Test Case 13：K=0^256, IV=0^96, 空明文/空 AAD，
    // 期望 tag = 530f8afbc74536b9a963b4f1c4cb738b
    const QByteArray zeroKey(32, '\0');
    const QByteArray zeroIv(12, '\0');
    QByteArray out;
    const bool sealed = FwCrypto::aesGcmSeal(zeroKey, zeroIv, QByteArray(), QByteArray(), &out);
    check(sealed && out == QByteArray::fromHex("530f8afbc74536b9a963b4f1c4cb738b"),
          "AES-256-GCM 空明文 KAT");

    // 随机往返
    const QByteArray key = randomBytes(32);
    const QByteArray iv = randomBytes(12);
    const QByteArray aad = QByteArrayLiteral("FWDA-v5-test-aad");
    const QByteArray plain = randomBytes(1000);
    QByteArray ct;
    check(FwCrypto::aesGcmSeal(key, iv, aad, plain, &ct), "GCM 加密");
    check(ct.size() == plain.size() + 16, "GCM 密文长度 = 明文 + 16");

    QByteArray back;
    check(FwCrypto::aesGcmOpen(key, iv, aad, ct, &back) && back == plain, "GCM 解密往返");

    // 篡改一个 bit
    QByteArray bad = ct;
    bad[bad.size() / 2] = char(bad[bad.size() / 2] ^ 0x01);
    check(!FwCrypto::aesGcmOpen(key, iv, aad, bad, &back), "GCM 检测密文篡改");

    // AAD 不匹配
    check(!FwCrypto::aesGcmOpen(key, iv, QByteArrayLiteral("other-aad"), ct, &back),
          "GCM 检测 AAD 不匹配");

    // 错误密钥
    const QByteArray wrongKey = randomBytes(32);
    check(!FwCrypto::aesGcmOpen(wrongKey, iv, aad, ct, &back), "GCM 检测错误密钥");
}

void testHkdf()
{
    // RFC 5869 Test Case 1
    const QByteArray ikm(22, '\x0b');
    const QByteArray salt = QByteArray::fromHex("000102030405060708090a0b0c");
    const QByteArray info = QByteArray::fromHex("f0f1f2f3f4f5f6f7f8f9");
    QByteArray okm;
    const bool ok = FwCrypto::hkdfSha256(ikm, salt, info, 42, &okm);
    check(ok && okm == QByteArray::fromHex(
                    "3cb25f25faacd57a90434f64d0362f2a"
                    "2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
                    "34007208d5b887185865"),
          "HKDF-SHA256 RFC 5869 用例 1");
}

void testArgon2Kat()
{
    QString err;
    check(FwCrypto::argon2SelfTest(&err), "Argon2id 官方 KAT 自检");
    if (!err.isEmpty())
        std::printf("        (%s)\n", qPrintable(err));
}

void testDerive()
{
    const QString userKey = QStringLiteral("FWDA-TEST-VECTOR");
    QByteArray k1, k2;
    QString err;

    QElapsedTimer timer;
    timer.start();
    const bool first = FwCrypto::deriveProductKey(userKey, &k1, &err);
    const qint64 elapsed = timer.elapsed();
    const bool second = FwCrypto::deriveProductKey(userKey, &k2, &err);

    check(first && second && k1.size() == 32, "Argon2id 派生 32 字节");
    check(k1 == k2, "同串派生结果确定");
    std::printf("        耗时 %lld ms，指纹 %s\n", static_cast<long long>(elapsed),
                qPrintable(FwCrypto::keyFingerprint(k1)));

    QByteArray unused;
    check(!FwCrypto::deriveProductKey(QString(), &unused, &err), "空串视为未配置");

    const QString fp = FwCrypto::keyFingerprint(k1);
    check(fp.size() == 8, "密钥指纹 8 位十六进制");
}

void testArchiveCrypto()
{
    QByteArray cmk, uuid;
    check(FwArcV5::newArchiveSecrets(&cmk, &uuid) && cmk.size() == 32 && uuid.size() == 16,
          "生成 CMK/UUID");

    const QByteArray productKey(32, 'K');
    QByteArray wrapped;
    check(FwArcV5::sealWrappedKey(productKey, cmk, uuid, &wrapped) && wrapped.size() == 60,
          "包裹 CMK（wrappedKey 60B）");
    QByteArray cmk2;
    check(FwArcV5::openWrappedKey(productKey, uuid, wrapped, &cmk2) && cmk2 == cmk,
          "解包 CMK 往返");
    const QByteArray wrongKey(32, 'X');
    check(!FwArcV5::openWrappedKey(wrongKey, uuid, wrapped, &cmk2), "错误 PRODUCT_KEY 被拒");

    // 索引
    const QByteArray plain = randomBytes(4096);
    const QByteArray iv = randomBytes(12);
    QByteArray cipher;
    check(FwArcV5::sealIndex(cmk, uuid, iv, plain, &cipher), "索引加密");
    QByteArray back;
    check(FwArcV5::openIndex(cmk, uuid, iv, cipher, &back) && back == plain, "索引解密往返");
    QByteArray bad = cipher;
    bad[bad.size() / 2] = char(bad[bad.size() / 2] ^ 0x01);
    check(!FwArcV5::openIndex(cmk, uuid, iv, bad, &back), "索引篡改被拒");

    // 内容块
    const QByteArray nonce = randomBytes(8);
    const QByteArray block = randomBytes(1024);
    QByteArray blockCt, blockBack;
    check(FwArcV5::sealChunk(cmk, uuid, 42, nonce, 0, 1, block, &blockCt), "内容块加密");
    check(FwArcV5::openChunk(cmk, uuid, 42, nonce, 0, 1, blockCt, &blockBack)
              && blockBack == block,
          "内容块解密往返");
    check(!FwArcV5::openChunk(cmk, uuid, 42, nonce, 1, 1, blockCt, &blockBack),
          "块号被篡改时拒绝");
    check(!FwArcV5::openChunk(cmk, uuid, 43, nonce, 0, 1, blockCt, &blockBack),
          "条目 ID 被篡改时拒绝");
}

void testKeyRing()
{
    QByteArray productKey(32, 'Z');
    FwCrypto::KeyRing::instance().setTestKey(productKey);

    QByteArray cmk, uuid;
    FwArcV5::newArchiveSecrets(&cmk, &uuid);
    QByteArray wrapped;
    FwArcV5::sealWrappedKey(productKey, cmk, uuid, &wrapped);

    QByteArray out;
    check(FwCrypto::KeyRing::instance().hasKey(), "密钥环已配置");
    check(FwCrypto::KeyRing::instance().unwrap(uuid, wrapped, &out) && out == cmk,
          "密钥环解包 CMK");

    FwCrypto::KeyRing::instance().setTestKey(QByteArray(32, 'Q'));
    check(!FwCrypto::KeyRing::instance().unwrap(uuid, wrapped, &out), "错误密钥环被拒");

    FwCrypto::KeyRing::instance().setTestKey(QByteArray());
}

void testIndex()
{
    QVector<FwArc::Entry> entries;
    const char *names[] = { "a.txt", "dir/b.bin", "中文/长名称.docx" };
    const quint64 sizes[] = { 123, (1ULL << 20) + 5, 0 };
    for (int i = 0; i < 3; ++i)
    {
        FwArc::Entry e;
        e.relPath     = QString::fromUtf8(names[i]);
        e.displayPath = e.relPath;
        e.entryId     = quint64(1000 + i);
        e.fileNonce   = randomBytes(8);
        e.size        = sizes[i];
        e.chunkCount  = FwArc::chunkCountFor(e.size);
        e.contentPos  = 100000 + quint64(i) * 1000;
        e.contentCrc  = 0xDEADBEEFu;
        e.createdMs   = 1700000000000ULL + quint64(i);
        e.modifiedMs  = e.createdMs + 1;
        e.accessedMs  = e.createdMs + 2;
        e.attributes  = 0x21;
        e.tags        = QStringList{ QStringLiteral("合同"), QStringLiteral("重要") };
        entries << e;
    }

    QString err;
    const qint64 exact = FwArcIndex::exactBytes(entries, QStringLiteral("D:/root"), &err);
    check(exact > 0, "索引精确大小");

    const qint64 capacity = exact + 4096;
    QByteArray plain;
    check(FwArcIndex::build(entries, QStringLiteral("D:/root"), capacity, &plain, &err),
          "索引序列化");
    check(plain.size() == capacity, "索引容量（含预留）");

    FwArc::Meta meta;
    check(FwArcIndex::parse(plain, &meta, &err), "索引解析");
    check(meta.entries.size() == 3 && meta.root == QStringLiteral("D:/root"),
          "索引根与条目数");
    check(meta.entries.at(2).relPath == QStringLiteral("中文/长名称.docx")
              && meta.entries.at(2).tags.size() == 2,
          "中文路径与标记往返");
    check(meta.entries.at(1).chunkCount == FwArc::chunkCountFor((1ULL << 20) + 5),
          "跨块条目块数往返");

    QByteArray bad = plain;
    bad[bad.size() - 1] = 'X';
    check(!FwArcIndex::parse(bad, &meta, &err), "索引预留区非零被拒");

    QByteArray small;
    check(!FwArcIndex::build(entries, QStringLiteral("D:/root"), exact - 1, &small, &err),
          "容量不足被拒");
}

} // namespace

int main()
{
    testArgon2Kat();
    testAesGcm();
    testHkdf();
    testDerive();
    testArchiveCrypto();
    testIndex();
    testKeyRing();

    std::printf("\n%s（%d 项失败）\n", g_fail == 0 ? "全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}

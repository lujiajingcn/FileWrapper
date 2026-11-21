#include "argonderive.h"
#include "cngcipher.h"

#include <QCryptographicHash>

extern "C" {
#include <argon2.h>
}

#include <cstring>

namespace FwCrypto {

bool deriveProductKey(const QString &userKey, QByteArray *pOut, QString *psErr)
{
    if (pOut == nullptr)
        return false;
    if (userKey.isEmpty()) {
        if (psErr) *psErr = QStringLiteral("密钥字符串为空");
        return false;
    }

    // NFC 规范化后再 UTF-8：保证同一串在不同输入法/平台上派生出同一把钥。
    const QByteArray pwd = userKey.normalized(QString::NormalizationForm_C).toUtf8();
    const QByteArray salt = QByteArray::fromRawData(kArgon2Salt, int(sizeof(kArgon2Salt) - 1));

    QByteArray out(kProductKeyLen, '\0');
    const int rc = argon2id_hash_raw(uint32_t(kArgon2Time), uint32_t(kArgon2MemoryKiB),
                                     uint32_t(kArgon2Lanes),
                                     pwd.constData(), size_t(pwd.size()),
                                     salt.constData(), size_t(salt.size()),
                                     out.data(), size_t(out.size()));
    if (rc != ARGON2_OK) {
        if (psErr) {
            *psErr = QStringLiteral("Argon2id 派生失败: %1")
                         .arg(QString::fromLatin1(argon2_error_message(rc)));
        }
        secureZero(out.data(), int(out.size()));
        return false;
    }

    *pOut = out;
    secureZero(out.data(), int(out.size()));
    return true;
}

bool argon2SelfTest(QString *psErr)
{
    // 官方 KAT（kats/argon2id，第一组）：Argon2id v1.3, t=3, m=32 KiB, p=4,
    // password=32×0x01, salt=16×0x02, secret=8×0x03, ad=12×0x04, tag=32B。
    static const unsigned char kExpected[32] = {
        0x0d, 0x64, 0x0d, 0xf5, 0x8d, 0x78, 0x76, 0x6c,
        0x08, 0xc0, 0x37, 0xa3, 0x4a, 0x8b, 0x53, 0xc9,
        0xd0, 0x1e, 0xf0, 0x45, 0x2d, 0x75, 0xb6, 0x5e,
        0xb5, 0x25, 0x20, 0xe9, 0x6b, 0x01, 0xe6, 0x59
    };

    unsigned char pwd[32];
    unsigned char salt[16];
    unsigned char secret[8];
    unsigned char ad[12];
    std::memset(pwd, 0x01, sizeof(pwd));
    std::memset(salt, 0x02, sizeof(salt));
    std::memset(secret, 0x03, sizeof(secret));
    std::memset(ad, 0x04, sizeof(ad));

    unsigned char out[32];
    std::memset(out, 0, sizeof(out));

    argon2_context ctx;
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.out = out;
    ctx.outlen = sizeof(out);
    ctx.pwd = pwd;
    ctx.pwdlen = sizeof(pwd);
    ctx.salt = salt;
    ctx.saltlen = sizeof(salt);
    ctx.secret = secret;
    ctx.secretlen = sizeof(secret);
    ctx.ad = ad;
    ctx.adlen = sizeof(ad);
    ctx.t_cost = 3;
    ctx.m_cost = 32;
    ctx.lanes = 4;
    ctx.threads = 4;
    ctx.version = ARGON2_VERSION_13;
    ctx.allocate_cbk = nullptr;
    ctx.free_cbk = nullptr;
    ctx.flags = ARGON2_DEFAULT_FLAGS;

    const int rc = argon2_ctx(&ctx, Argon2_id);
    if (rc != ARGON2_OK) {
        if (psErr) {
            *psErr = QStringLiteral("Argon2 自检调用失败: %1")
                         .arg(QString::fromLatin1(argon2_error_message(rc)));
        }
        return false;
    }

    if (std::memcmp(out, kExpected, sizeof(kExpected)) != 0) {
        if (psErr) *psErr = QStringLiteral("Argon2 自检结果与官方 KAT 不一致");
        return false;
    }
    return true;
}

QString keyFingerprint(const QByteArray &productKey)
{
    const QByteArray h = QCryptographicHash::hash(productKey, QCryptographicHash::Sha256);
    return QString::fromLatin1(h.left(4).toHex().toUpper());
}

} // namespace FwCrypto

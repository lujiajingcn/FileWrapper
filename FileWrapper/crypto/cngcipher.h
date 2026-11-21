#ifndef FWCRYPTO_CNGCIPHER_H
#define FWCRYPTO_CNGCIPHER_H

// ============================================================
// Windows CNG 加密原语封装（FWDA v5 加密层的基础设施）
//
//   * AES-256-GCM 封装/解封（含 16 字节认证 tag）
//   * HKDF-SHA256（RFC 5869）
//   * CSPRNG（BCryptGenRandom）
//   * 内存安全清零
//
// 只做密码学原语，不含任何归档格式知识；格式相关逻辑见 archivecrypto。
// ============================================================

#include <QByteArray>

namespace FwCrypto {

// AES-256-GCM 固定参数
constexpr int kAesKeyLen = 32;
constexpr int kGcmIvLen  = 12;
constexpr int kGcmTagLen = 16;

// 生成 nLen 字节密码学随机数；失败返回 false。
bool randomBytes(void *pOut, int nLen);

// AES-256-GCM 加密：pOut = ciphertext || tag(16B)。
// key 必须 32B，iv 必须 12B；aad 可为空。
bool aesGcmSeal(const QByteArray &key, const QByteArray &iv, const QByteArray &aad,
                const QByteArray &plain, QByteArray *pOut);

// AES-256-GCM 解密：入参为 ciphertext || tag(16B)，认证失败返回 false。
bool aesGcmOpen(const QByteArray &key, const QByteArray &iv, const QByteArray &aad,
                const QByteArray &cipherWithTag, QByteArray *pOut);

// HKDF-SHA256（RFC 5869）。outLen ≤ 255*32。
bool hkdfSha256(const QByteArray &ikm, const QByteArray &salt,
                const QByteArray &info, int outLen, QByteArray *pOut);

// 抗优化清零。
void secureZero(void *p, int nLen);

} // namespace FwCrypto

#endif // FWCRYPTO_CNGCIPHER_H

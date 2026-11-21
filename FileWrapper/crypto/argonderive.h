#ifndef FWCRYPTO_ARGONDERIVE_H
#define FWCRYPTO_ARGONDERIVE_H

// ============================================================
// 用户密钥字符串 → PRODUCT_KEY（FWDA v5）
//
//   PRODUCT_KEY = Argon2id(UTF-8(NFC(用户字符串)),
//                          salt = "FWDA/v5/product-key", 固定常量,
//                          m = 262144 KiB (256 MiB), t = 4, p = 4) → 32B
//
// 参数与盐属于**冻结范围**：任何改动都会让旧归档无法打开。
// 实现基于 third_party/argon2（phc-winner-argon2 20190702，CC0/Apache-2.0）。
// ============================================================

#include <QByteArray>
#include <QString>

namespace FwCrypto {

// 冻结参数（见设计文档 §4.2 / §16）
constexpr int      kArgon2MemoryKiB = 262144;   // 256 MiB
constexpr int      kArgon2Time      = 4;
constexpr int      kArgon2Lanes     = 4;
constexpr int      kProductKeyLen   = 32;
constexpr char     kArgon2Salt[]    = "FWDA/v5/product-key";

// 任意非空字符串 → 32 字节 PRODUCT_KEY。
// 空字符串视为未配置，返回 false（不算错误，调用方应先拦截）。
bool deriveProductKey(const QString &userKey, QByteArray *pOut,
                      QString *psErr = nullptr);

// 用官方 KAT 验证 Argon2id 实现正确性（t=3,m=32,p=4, 带 secret/ad, v=0x13）。
// 应在启动自检或单测中调用一次；失败说明编译/链接实现有问题。
bool argon2SelfTest(QString *psErr = nullptr);

// 密钥指纹：SHA-256(PRODUCT_KEY) 前 4 字节，8 位大写十六进制。
// 只用于多机核对，不降低离线爆破难度（算指纹本身就要先跑 Argon2id）。
QString keyFingerprint(const QByteArray &productKey);

} // namespace FwCrypto

#endif // FWCRYPTO_ARGONDERIVE_H

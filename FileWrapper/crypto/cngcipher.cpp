#include "cngcipher.h"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>

#include <windows.h>
#include <bcrypt.h>

#ifndef BCRYPT_SUCCESS
#define BCRYPT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif

namespace FwCrypto {

namespace {

// RAII：算法句柄
class AlgHandle
{
public:
    AlgHandle() = default;
    ~AlgHandle() { if (m_h) BCryptCloseAlgorithmProvider(m_h, 0); }
    AlgHandle(const AlgHandle &) = delete;
    AlgHandle &operator=(const AlgHandle &) = delete;
    BCRYPT_ALG_HANDLE get() const { return m_h; }
    BCRYPT_ALG_HANDLE *addr() { return &m_h; }
private:
    BCRYPT_ALG_HANDLE m_h = nullptr;
};

// RAII：对称密钥句柄
class KeyHandle
{
public:
    KeyHandle() = default;
    ~KeyHandle() { if (m_h) BCryptDestroyKey(m_h); }
    KeyHandle(const KeyHandle &) = delete;
    KeyHandle &operator=(const KeyHandle &) = delete;
    void reset(BCRYPT_KEY_HANDLE h)
    {
        if (m_h)
            BCryptDestroyKey(m_h);
        m_h = h;
    }
    BCRYPT_KEY_HANDLE get() const { return m_h; }
private:
    BCRYPT_KEY_HANDLE m_h = nullptr;
};

inline PUCHAR puchar(const QByteArray &a)
{
    return reinterpret_cast<PUCHAR>(const_cast<char *>(a.constData()));
}

// 打开 AES-GCM 算法并生成对称密钥；成功返回 true。
bool openGcmKey(const QByteArray &key, AlgHandle &alg, KeyHandle &keyHandle)
{
    if (key.size() != kAesKeyLen)
        return false;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(alg.addr(), BCRYPT_AES_ALGORITHM,
                                                    nullptr, 0)))
        return false;
    if (!BCRYPT_SUCCESS(BCryptSetProperty(alg.get(), BCRYPT_CHAINING_MODE,
                                          reinterpret_cast<PUCHAR>(
                                              const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_GCM)),
                                          sizeof(BCRYPT_CHAIN_MODE_GCM), 0)))
        return false;

    BCRYPT_KEY_HANDLE h = nullptr;
    if (!BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(alg.get(), &h, nullptr, 0,
                                                   puchar(key), ULONG(key.size()), 0)))
        return false;
    keyHandle.reset(h);
    return true;
}

// 填充 GCM 认证信息。tag 指向 16 字节缓冲区。
void initAuthInfo(BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO &info,
                  const QByteArray &iv, const QByteArray &aad, PUCHAR tag)
{
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce    = puchar(iv);
    info.cbNonce    = ULONG(iv.size());
    info.pbAuthData = aad.isEmpty() ? nullptr : puchar(aad);
    info.cbAuthData = ULONG(aad.size());
    info.pbTag      = tag;
    info.cbTag      = kGcmTagLen;
}

} // namespace

bool randomBytes(void *pOut, int nLen)
{
    if (pOut == nullptr || nLen <= 0)
        return false;
    return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(pOut),
                                          ULONG(nLen), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

bool aesGcmSeal(const QByteArray &key, const QByteArray &iv, const QByteArray &aad,
                const QByteArray &plain, QByteArray *pOut)
{
    if (pOut == nullptr || iv.size() != kGcmIvLen)
        return false;

    AlgHandle alg;
    KeyHandle keyHandle;
    if (!openGcmKey(key, alg, keyHandle))
        return false;

    QByteArray out(int(plain.size()) + kGcmTagLen, '\0');
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    initAuthInfo(info, iv, aad, reinterpret_cast<PUCHAR>(out.data()) + plain.size());

    ULONG written = 0;
    const NTSTATUS st = BCryptEncrypt(
        keyHandle.get(),
        plain.isEmpty() ? nullptr : puchar(plain), ULONG(plain.size()),
        &info, nullptr, 0,
        reinterpret_cast<PUCHAR>(out.data()), ULONG(out.size()),
        &written, 0);
    if (!BCRYPT_SUCCESS(st) || written != ULONG(plain.size()))
        return false;

    *pOut = out;
    return true;
}

bool aesGcmOpen(const QByteArray &key, const QByteArray &iv, const QByteArray &aad,
                const QByteArray &cipherWithTag, QByteArray *pOut)
{
    if (pOut == nullptr || iv.size() != kGcmIvLen
        || cipherWithTag.size() < kGcmTagLen)
        return false;

    const int ctLen = int(cipherWithTag.size()) - kGcmTagLen;

    AlgHandle alg;
    KeyHandle keyHandle;
    if (!openGcmKey(key, alg, keyHandle))
        return false;

    QByteArray tag = cipherWithTag.right(kGcmTagLen);
    QByteArray plain(ctLen, '\0');

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    initAuthInfo(info, iv, aad, reinterpret_cast<PUCHAR>(tag.data()));

    ULONG written = 0;
    const NTSTATUS st = BCryptDecrypt(
        keyHandle.get(),
        ctLen == 0 ? nullptr : puchar(cipherWithTag.left(ctLen)), ULONG(ctLen),
        &info, nullptr, 0,
        ctLen == 0 ? nullptr : reinterpret_cast<PUCHAR>(plain.data()), ULONG(ctLen),
        &written, 0);
    if (!BCRYPT_SUCCESS(st) || written != ULONG(ctLen))
        return false;

    *pOut = plain;
    return true;
}

bool hkdfSha256(const QByteArray &ikm, const QByteArray &salt,
                const QByteArray &info, int outLen, QByteArray *pOut)
{
    if (pOut == nullptr || outLen <= 0 || outLen > 255 * 32)
        return false;

    const QByteArray realSalt = salt.isEmpty() ? QByteArray(32, '\0') : salt;

    QMessageAuthenticationCode extract(QCryptographicHash::Sha256, realSalt);
    extract.addData(ikm);
    QByteArray prk = extract.result();

    QByteArray okm;
    QByteArray t;
    for (int i = 1; okm.size() < outLen; ++i) {
        QMessageAuthenticationCode expand(QCryptographicHash::Sha256, prk);
        expand.addData(t);
        expand.addData(info);
        const char ctr = char(i);
        expand.addData(&ctr, 1);
        t = expand.result();
        okm.append(t);
    }
    okm.truncate(outLen);

    secureZero(prk.data(), int(prk.size()));
    secureZero(t.data(), int(t.size()));
    *pOut = okm;
    return true;
}

void secureZero(void *p, int nLen)
{
    if (p != nullptr && nLen > 0)
        SecureZeroMemory(p, SIZE_T(nLen));
}

} // namespace FwCrypto

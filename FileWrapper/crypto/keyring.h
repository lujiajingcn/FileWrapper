#ifndef FWCRYPTO_KEYRING_H
#define FWCRYPTO_KEYRING_H

// ============================================================
// 密钥环：用户配置的密钥字符串 → PRODUCT_KEY，以及历史密钥
//
//   * 存储：QSettings INI（%APPDATA%\FileWrapper\FileWrapper.ini）
//           [Crypto] KeyRing = "指纹|v1:<base64(DPAPI 密文)>", ...
//   * DPAPI：CryptProtectData（当前 Windows 用户，附加固定熵）
//   * 派生：Argon2id（crypto/argonderive），会话内缓存
//   * 打开归档时按“当前 → 历史”顺序尝试解 wrappedKey
//
// 程序本身不保存任何密钥；换机器/换 Windows 用户必须重新输入。
// ============================================================

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace FwCrypto {

class KeyRing
{
public:
    static KeyRing &instance();

    // 重新从 QSettings 载入（设置保存后/测试重置后）。
    void reloadFromStorage();

    // 是否配置过密钥（测试注入也算）。
    bool hasKey();

    // 当前生效的 PRODUCT_KEY；未配置或派生失败返回空。
    QByteArray currentKey();

    // 当前密钥指纹（8 位十六进制）；未配置返回空。
    QString currentFingerprint();

    // 环内指纹列表（当前在前）。
    QStringList fingerprints();

    // 应用新的密钥字符串：派生 → DPAPI 加密 → 写入环首（去重、最多 8 条）。
    // 空字符串视为“清除当前”。
    bool setCurrentKey(const QString &userKey, QString *psErr = nullptr);

    // 移除当前密钥（下一条历史密钥成为当前）；没有剩余时视为未配置。
    bool removeCurrentKey(QString *psErr = nullptr);

    // 清空全部密钥。
    void clearAllKeys();

    // 依次用密钥环里的每个 PRODUCT_KEY 解 wrappedKey；成功输出 CMK。
    bool unwrap(const QByteArray &uuid, const QByteArray &wrappedKey,
                QByteArray *cmk, QString *psErr = nullptr);

    // 单测注入：直接指定 PRODUCT_KEY（不落盘、不派生）；传空取消。
    void setTestKey(const QByteArray &productKey);

private:
    KeyRing() = default;

    struct Item
    {
        QString    fp;        // 指纹（8 位十六进制）
        QString    userKey;   // 用户字符串（内存中；DPAPI 解密得到）
        QByteArray derived;   // 派生结果缓存
        bool       derivedOk = false;
    };

    void ensureLoaded();
    bool deriveItem(Item &item);
    bool writeRing(QString *psErr);
    static bool dpapiProtect(const QByteArray &plain, QByteArray *out, QString *psErr);
    static bool dpapiUnprotect(const QByteArray &blob, QByteArray *out, QString *psErr);

    bool                 m_loaded = false;
    QVector<Item>        m_items;
    QByteArray           m_testKey;
};

} // namespace FwCrypto

#endif // FWCRYPTO_KEYRING_H

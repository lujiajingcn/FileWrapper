#include "keyring.h"

#include "archivecrypto.h"
#include "argonderive.h"
#include "cngcipher.h"

#include <QSettings>
#include <QVector>

#include <windows.h>
#include <wincrypt.h>

namespace FwCrypto {

namespace {

constexpr int kMaxRing = 8;
const char  kEntropy[] = "FileWrapper/v5/keyring/v1";
const char  kSettingsGroup[] = "Crypto";
const char  kSettingsKey[]   = "KeyRing";

} // namespace

KeyRing &KeyRing::instance()
{
    static KeyRing s_ring;
    return s_ring;
}

void KeyRing::reloadFromStorage()
{
    m_items.clear();
    m_loaded = true;

    QSettings st(QSettings::IniFormat, QSettings::UserScope,
                 QStringLiteral("FileWrapper"), QStringLiteral("FileWrapper"));
    st.beginGroup(QLatin1String(kSettingsGroup));
    const QStringList raw = st.value(QLatin1String(kSettingsKey)).toStringList();
    st.endGroup();

    for (const QString &line : raw)
    {
        const int sep = line.indexOf(QLatin1Char('|'));
        const int prefix = line.indexOf(QLatin1String("v1:"));
        if (sep <= 0 || prefix < 0)
            continue;
        const QString fp = line.left(sep);
        const QByteArray blob = QByteArray::fromBase64(line.mid(prefix + 3).toLatin1());
        QByteArray plain;
        QString err;
        if (!dpapiUnprotect(blob, &plain, &err))
            continue;
        Item it;
        it.fp = fp;
        it.userKey = QString::fromUtf8(plain);
        secureZero(plain.data(), int(plain.size()));
        if (!it.userKey.isEmpty())
            m_items.append(it);
    }
}

void KeyRing::ensureLoaded()
{
    if (!m_loaded)
        reloadFromStorage();
}

bool KeyRing::hasKey()
{
    ensureLoaded();
    return !m_testKey.isEmpty() || !m_items.isEmpty();
}

bool KeyRing::deriveItem(Item &item)
{
    if (item.derivedOk)
        return !item.derived.isEmpty();
    QString err;
    if (!deriveProductKey(item.userKey, &item.derived, &err))
    {
        item.derivedOk = false;
        return false;
    }
    item.derivedOk = true;
    return true;
}

QByteArray KeyRing::currentKey()
{
    ensureLoaded();
    if (!m_testKey.isEmpty())
        return m_testKey;
    if (m_items.isEmpty())
        return QByteArray();
    if (!deriveItem(m_items[0]))
        return QByteArray();
    return m_items[0].derived;
}

QString KeyRing::currentFingerprint()
{
    const QByteArray k = currentKey();
    return k.isEmpty() ? QString() : keyFingerprint(k);
}

QStringList KeyRing::fingerprints()
{
    ensureLoaded();
    QStringList out;
    for (const Item &it : m_items)
        out << it.fp;
    return out;
}

bool KeyRing::setCurrentKey(const QString &userKey, QString *psErr)
{
    ensureLoaded();
    if (userKey.isEmpty())
        return removeCurrentKey(psErr);

    QByteArray derived;
    QString err;
    if (!deriveProductKey(userKey, &derived, &err))
    {
        if (psErr) *psErr = err;
        return false;
    }
    const QString fp = keyFingerprint(derived);
    secureZero(derived.data(), int(derived.size()));

    QByteArray blob;
    const QByteArray plain = userKey.toUtf8();
    if (!dpapiProtect(plain, &blob, &err))
    {
        if (psErr) *psErr = err;
        return false;
    }

    // 去重后插到最前，最多 8 条。
    QVector<Item> next;
    Item cur;
    cur.fp = fp;
    cur.userKey = userKey;
    next.append(cur);
    for (const Item &it : m_items)
    {
        if (it.fp != fp && next.size() < kMaxRing)
            next.append(it);
    }
    m_items = next;
    if (!writeRing(psErr))
        return false;
    return true;
}

bool KeyRing::removeCurrentKey(QString *psErr)
{
    ensureLoaded();
    if (m_testKey.isEmpty() && m_items.isEmpty())
        return true;
    if (m_testKey.isEmpty() && !m_items.isEmpty())
        m_items.removeFirst();
    return writeRing(psErr);
}

void KeyRing::clearAllKeys()
{
    m_items.clear();
    m_testKey.clear();
    QString err;
    writeRing(&err);
}

bool KeyRing::writeRing(QString *psErr)
{
    QStringList raw;
    for (const Item &it : m_items)
    {
        QByteArray blob;
        QString err;
        const QByteArray plain = it.userKey.toUtf8();
        if (!dpapiProtect(plain, &blob, &err))
        {
            if (psErr) *psErr = err;
            return false;
        }
        raw << QStringLiteral("%1|v1:%2").arg(it.fp, QString::fromLatin1(blob.toBase64()));
    }

    QSettings st(QSettings::IniFormat, QSettings::UserScope,
                 QStringLiteral("FileWrapper"), QStringLiteral("FileWrapper"));
    st.beginGroup(QLatin1String(kSettingsGroup));
    if (raw.isEmpty())
        st.remove(QLatin1String(kSettingsKey));
    else
        st.setValue(QLatin1String(kSettingsKey), raw);
    st.endGroup();
    st.sync();
    if (st.status() != QSettings::NoError)
    {
        if (psErr) *psErr = QStringLiteral("写入设置失败");
        return false;
    }
    m_loaded = true;
    return true;
}

bool KeyRing::unwrap(const QByteArray &uuid, const QByteArray &wrappedKey,
                     QByteArray *cmk, QString *psErr)
{
    ensureLoaded();
    if (cmk == nullptr)
        return false;

    QVector<QByteArray> candidates;
    if (!m_testKey.isEmpty())
        candidates.append(m_testKey);
    for (Item &it : m_items)
    {
        if (deriveItem(it) && !it.derived.isEmpty())
            candidates.append(it.derived);
    }

    for (const QByteArray &key : candidates)
    {
        QByteArray out;
        QString err;
        if (FwArcV5::openWrappedKey(key, uuid, wrappedKey, &out, &err))
        {
            *cmk = out;
            return true;
        }
    }

    if (psErr) {
        *psErr = candidates.isEmpty()
                     ? QStringLiteral("尚未配置密钥：请在“设置 → 归档加密密钥”中输入密钥字符串")
                     : QStringLiteral("没有匹配的密钥：请检查设置中的密钥字符串，或确认归档已损坏");
    }
    return false;
}

void KeyRing::setTestKey(const QByteArray &productKey)
{
    m_testKey = productKey;
    if (!productKey.isEmpty())
        m_loaded = true;
}

bool KeyRing::dpapiProtect(const QByteArray &plain, QByteArray *out, QString *psErr)
{
    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(plain.constData()));
    in.cbData = DWORD(plain.size());
    DATA_BLOB entropy;
    entropy.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(kEntropy));
    entropy.cbData = DWORD(sizeof(kEntropy) - 1);
    DATA_BLOB blob{};
    if (!CryptProtectData(&in, L"FileWrapper key", &entropy, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &blob)) {
        if (psErr) *psErr = QStringLiteral("DPAPI 加密密钥失败（错误码 %1）").arg(GetLastError());
        return false;
    }
    *out = QByteArray(reinterpret_cast<const char *>(blob.pbData), int(blob.cbData));
    LocalFree(blob.pbData);
    return true;
}

bool KeyRing::dpapiUnprotect(const QByteArray &blob, QByteArray *out, QString *psErr)
{
    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(blob.constData()));
    in.cbData = DWORD(blob.size());
    DATA_BLOB entropy;
    entropy.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(kEntropy));
    entropy.cbData = DWORD(sizeof(kEntropy) - 1);
    DATA_BLOB plain{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &plain)) {
        if (psErr) *psErr = QStringLiteral("DPAPI 解密密钥失败（错误码 %1）").arg(GetLastError());
        return false;
    }
    *out = QByteArray(reinterpret_cast<const char *>(plain.pbData), int(plain.cbData));
    LocalFree(plain.pbData);
    return true;
}

} // namespace FwCrypto

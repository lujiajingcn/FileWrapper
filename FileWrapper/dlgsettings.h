#ifndef DLGSETTINGS_H
#define DLGSETTINGS_H

// ============================================================
// 设置 · 归档加密密钥
//
// 用户输入任意字符串 → Argon2id 派生 PRODUCT_KEY → DPAPI 加密存入
// QSettings 的密钥环（当前 + 历史，最多 8 条）。打开归档时按密钥环顺序尝试。
// 程序本身不保存任何密钥；换机器/换 Windows 用户必须重新输入。
// ============================================================

#include <QDialog>

class QLineEdit;
class QLabel;
class QListWidget;
class QCheckBox;
class QPushButton;

class DlgSettings : public QDialog
{
    Q_OBJECT

public:
    explicit DlgSettings(QWidget *parent = nullptr);

private slots:
    void onApply();
    void onRemoveCurrent();
    void onClearAll();
    void onToggleReveal(bool checked);
    void onTextChanged(const QString &text);

private:
    void refreshRing();
    static QString strengthHint(const QString &text);

    QLineEdit   *m_editKey       = nullptr;
    QCheckBox   *m_chkReveal     = nullptr;
    QLabel      *m_lblStrength   = nullptr;
    QLabel      *m_lblFingerprint = nullptr;
    QLabel      *m_lblStatus     = nullptr;
    QListWidget *m_listRing      = nullptr;
    QPushButton *m_btnRemove     = nullptr;
    QPushButton *m_btnClear      = nullptr;
};

#endif // DLGSETTINGS_H

#include "dlgsettings.h"

#include "crypto/keyring.h"

#include <QApplication>
#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

DlgSettings::DlgSettings(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("设置 · 归档加密密钥"));
    setMinimumWidth(520);

    auto *pRoot = new QVBoxLayout(this);

    auto *pIntro = new QLabel(
        tr("归档内容使用 AES-256 加密。程序会把下面这串文字经 Argon2id 派生为密钥；"
           "程序本身不保存密钥，换机器或换 Windows 用户需要重新输入。"), this);
    pIntro->setWordWrap(true);
    pRoot->addWidget(pIntro);

    // —— 当前密钥输入 ——
    auto *pForm = new QFormLayout();
    auto *pKeyRow = new QHBoxLayout();
    m_editKey = new QLineEdit(this);
    m_editKey->setEchoMode(QLineEdit::Password);
    m_editKey->setPlaceholderText(tr("输入任意字符串（不限制内容）"));
    m_chkReveal = new QCheckBox(tr("显示"), this);
    pKeyRow->addWidget(m_editKey, 1);
    pKeyRow->addWidget(m_chkReveal);
    pForm->addRow(tr("密钥字符串："), pKeyRow);

    m_lblStrength = new QLabel(tr("强度：—"), this);
    pForm->addRow(QString(), m_lblStrength);
    pRoot->addLayout(pForm);

    m_lblFingerprint = new QLabel(this);
    m_lblFingerprint->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pRoot->addWidget(m_lblFingerprint);

    auto *pWarn = new QLabel(
        tr("⚠ 字符串强度就是文件的安全强度：短串或常见短语可被离线穷举。建议不少于 16 位随机字符。\n"
           "⚠ 字符串丢失后，用它加密的归档将无法打开，请自行妥善备份。"), this);
    pWarn->setWordWrap(true);
    pRoot->addWidget(pWarn);

    // —— 历史密钥 ——
    pRoot->addWidget(new QLabel(tr("历史密钥（打开旧归档时依次尝试，最多 8 条）："), this));
    m_listRing = new QListWidget(this);
    m_listRing->setMaximumHeight(130);
    pRoot->addWidget(m_listRing);

    auto *pBtns = new QHBoxLayout();
    auto *pApply = new QPushButton(tr("应用"), this);
    m_btnRemove = new QPushButton(tr("删除当前"), this);
    m_btnClear = new QPushButton(tr("清空全部"), this);
    auto *pClose = new QPushButton(tr("关闭"), this);
    pBtns->addWidget(pApply);
    pBtns->addWidget(m_btnRemove);
    pBtns->addWidget(m_btnClear);
    pBtns->addStretch(1);
    pBtns->addWidget(pClose);
    pRoot->addLayout(pBtns);

    m_lblStatus = new QLabel(this);
    m_lblStatus->setWordWrap(true);
    pRoot->addWidget(m_lblStatus);

    connect(m_chkReveal, &QCheckBox::toggled, this, &DlgSettings::onToggleReveal);
    connect(m_editKey, &QLineEdit::textChanged, this, &DlgSettings::onTextChanged);
    connect(pApply, &QPushButton::clicked, this, &DlgSettings::onApply);
    connect(m_btnRemove, &QPushButton::clicked, this, &DlgSettings::onRemoveCurrent);
    connect(m_btnClear, &QPushButton::clicked, this, &DlgSettings::onClearAll);
    connect(pClose, &QPushButton::clicked, this, &QDialog::accept);

    refreshRing();
}

void DlgSettings::refreshRing()
{
    const QStringList ring = FwCrypto::KeyRing::instance().fingerprints();
    const QString current = FwCrypto::KeyRing::instance().currentFingerprint();

    m_listRing->clear();
    if (ring.isEmpty())
    {
        m_listRing->addItem(tr("（尚未配置）"));
        m_lblFingerprint->setText(tr("当前状态：未配置"));
        m_lblStatus->setText(tr("未配置密钥时无法合并文件。"));
    }
    else
    {
        for (const QString &fp : ring)
            m_listRing->addItem(fp == current ? tr("%1    （当前）").arg(fp) : fp);
        m_lblFingerprint->setText(tr("当前密钥指纹：%1").arg(current));
        m_lblStatus->setText(tr("已配置 %1 条密钥。").arg(ring.size()));
    }
    m_btnRemove->setEnabled(!ring.isEmpty());
    m_btnClear->setEnabled(!ring.isEmpty());
}

void DlgSettings::onTextChanged(const QString &text)
{
    m_lblStrength->setText(tr("强度：%1").arg(strengthHint(text)));
}

QString DlgSettings::strengthHint(const QString &text)
{
    if (text.isEmpty())
        return tr("—");
    int classes = 0;
    if (text.contains(QRegularExpression(QStringLiteral("[a-z]")))) ++classes;
    if (text.contains(QRegularExpression(QStringLiteral("[A-Z]")))) ++classes;
    if (text.contains(QRegularExpression(QStringLiteral("[0-9]")))) ++classes;
    if (text.contains(QRegularExpression(QStringLiteral("[^A-Za-z0-9]")))) ++classes;

    const int n = text.size();
    if (n < 8)
        return tr("弱（建议至少 16 位随机字符）");
    if (n < 12 || classes <= 1)
        return tr("中（建议至少 16 位随机字符）");
    if (n < 16)
        return tr("较强（16 位以上随机字符更稳妥）");
    return tr("强（请确认不是常见短语）");
}

void DlgSettings::onToggleReveal(bool checked)
{
    m_editKey->setEchoMode(checked ? QLineEdit::Normal : QLineEdit::Password);
}

void DlgSettings::onApply()
{
    const QString text = m_editKey->text();
    if (text.isEmpty())
    {
        QMessageBox::information(this, tr("应用密钥"),
                                 tr("字符串为空。要清除当前密钥，请使用「删除当前」。"));
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString err;
    const bool ok = FwCrypto::KeyRing::instance().setCurrentKey(text, &err);
    QApplication::restoreOverrideCursor();

    if (!ok)
    {
        QMessageBox::warning(this, tr("应用密钥失败"), err);
        return;
    }
    m_editKey->clear();
    refreshRing();
    m_lblStatus->setText(tr("密钥已应用。注意：此前的归档仍需要旧密钥（已保留在历史密钥中）。"));
}

void DlgSettings::onRemoveCurrent()
{
    const auto ret = QMessageBox::question(
        this, tr("删除当前密钥"),
        tr("删除当前密钥后，新合并将使用下一条历史密钥（没有则视为未配置）。\n"
           "已经用当前密钥加密的归档不会受影响，但需要重新配置该密钥才能打开。\n\n继续？"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret != QMessageBox::Yes)
        return;

    QString err;
    if (!FwCrypto::KeyRing::instance().removeCurrentKey(&err))
    {
        QMessageBox::warning(this, tr("删除失败"), err);
        return;
    }
    refreshRing();
}

void DlgSettings::onClearAll()
{
    const auto ret = QMessageBox::warning(
        this, tr("清空全部密钥"),
        tr("清空后将无法打开任何用这些密钥加密的归档（除非重新输入对应字符串）。\n\n确认清空？"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret != QMessageBox::Yes)
        return;
    FwCrypto::KeyRing::instance().clearAllKeys();
    refreshRing();
}

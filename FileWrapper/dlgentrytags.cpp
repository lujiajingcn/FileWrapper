#include "dlgentrytags.h"
#include "archiveformat.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

// 输入框里的多个标记用这些字符分隔。中文标点也认，避免用户从文档里复制过来
// 一堆全角逗号时只得到一个超长标记。
QStringList splitTagInput(const QString &sText)
{
    QString s = sText;
    static const QChar kSeparators[] = {
        QLatin1Char(','), QChar(ushort(0xFF0C)),   // , ，
        QLatin1Char(';'), QChar(ushort(0xFF1B)),   // ; ；
        QChar(ushort(0x3001)),                     // 、
        QLatin1Char('\n')
    };
    for (const QChar sep : kSeparators)
        s.replace(sep, QLatin1Char('\n'));
    return s.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

} // namespace

DlgEntryTags::DlgEntryTags(const QString &sFileName, const QStringList &knownTags,
                           const QStringList &currentTags, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("标记 - %1").arg(sFileName));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    resize(480, 430);

    auto *pRoot = new QVBoxLayout(this);
    pRoot->setContentsMargins(18, 16, 18, 14);
    pRoot->setSpacing(10);

    // 标题只显示文件名：对话框标题栏已经带完整名字，长路径放在 tooltip 里。
    auto *pName = new QLabel(sFileName, this);
    pName->setObjectName(QStringLiteral("lblTagFileName"));
    pName->setTextFormat(Qt::PlainText);
    pName->setToolTip(sFileName);
    pName->setStyleSheet(QStringLiteral("font-weight:600;"));
    pName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    pRoot->addWidget(pName);

    auto *pHint = new QLabel(
        tr("勾选已有标记，或在下方输入新标记（逗号、分号或顿号分隔）。\n"
           "标记保存在归档内，会随 .fwda 文件一起分发；最多 %1 个，单个最多 %2 个字符。")
            .arg(int(FwArc::MAX_TAGS))
            .arg(int(FwArc::MAX_TAG_CHARS)),
        this);
    pHint->setObjectName(QStringLiteral("lblTagHint"));
    pHint->setWordWrap(true);
    pHint->setStyleSheet(QStringLiteral("color:#6B7280;"));
    pRoot->addWidget(pHint);

    m_pList = new QListWidget(this);
    m_pList->setObjectName(QStringLiteral("listTags"));
    m_pList->setSelectionMode(QAbstractItemView::NoSelection);
    m_pList->setAlternatingRowColors(true);
    m_pList->setToolTip(tr("勾选要应用到该文件的标记"));
    pRoot->addWidget(m_pList, 1);

    auto *pAddRow = new QHBoxLayout;
    pAddRow->setContentsMargins(0, 0, 0, 0);
    pAddRow->setSpacing(8);
    m_pEdit = new QLineEdit(this);
    m_pEdit->setObjectName(QStringLiteral("leNewTag"));
    m_pEdit->setPlaceholderText(tr("新标记，例如：合同、重要"));
    m_pEdit->setClearButtonEnabled(true);
    m_pAdd = new QPushButton(tr("添加"), this);
    m_pAdd->setObjectName(QStringLiteral("btnAddTag"));
    m_pAdd->setAutoDefault(false);
    pAddRow->addWidget(m_pEdit, 1);
    pAddRow->addWidget(m_pAdd);
    pRoot->addLayout(pAddRow);

    auto *pButtons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    pButtons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("btnOkTags"));
    pButtons->button(QDialogButtonBox::Cancel)->setObjectName(QStringLiteral("btnCancelTags"));
    pButtons->button(QDialogButtonBox::Ok)->setText(tr("确定"));
    pButtons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    pButtons->button(QDialogButtonBox::Ok)->setDefault(true);
    pRoot->addWidget(pButtons);

    connect(pButtons, &QDialogButtonBox::accepted, this, [this]() { accept(); });
    connect(pButtons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_pAdd, &QPushButton::clicked, this, &DlgEntryTags::addTypedTags);
    connect(m_pEdit, &QLineEdit::returnPressed, this, &DlgEntryTags::addTypedTags);

    // 候选 = 当前标记 + 归档里其它标记（大小写不敏感去重，保留已有写法）。
    QStringList candidates = currentTags;
    for (const QString &sTag : knownTags)
    {
        bool bExists = false;
        for (const QString &sKnown : candidates)
        {
            if (sKnown.compare(sTag, Qt::CaseInsensitive) == 0)
            {
                bExists = true;
                break;
            }
        }
        if (!bExists)
            candidates << sTag;
    }
    for (const QString &sTag : candidates)
    {
        bool bChecked = false;
        for (const QString &sCurrent : currentTags)
        {
            if (sCurrent.compare(sTag, Qt::CaseInsensitive) == 0)
            {
                bChecked = true;
                break;
            }
        }
        addCandidate(sTag, bChecked);
    }
}

void DlgEntryTags::addCandidate(const QString &sTag, bool bChecked)
{
    auto *pItem = new QListWidgetItem(sTag, m_pList);
    pItem->setFlags(pItem->flags() | Qt::ItemIsUserCheckable);
    pItem->setCheckState(bChecked ? Qt::Checked : Qt::Unchecked);
}

void DlgEntryTags::addTypedTags()
{
    const QStringList parts = splitTagInput(m_pEdit->text());
    if (parts.isEmpty())
        return;

    QString sErr;
    const QStringList newTags = FwArc::normalizeTags(parts, &sErr);
    if (!sErr.isEmpty())
    {
        QMessageBox::warning(this, tr("标记无效"), sErr);
        return;
    }

    for (const QString &sTag : newTags)
    {
        bool bExists = false;
        for (int i = 0; i < m_pList->count(); ++i)
        {
            QListWidgetItem *pItem = m_pList->item(i);
            if (pItem != nullptr && pItem->text().compare(sTag, Qt::CaseInsensitive) == 0)
            {
                pItem->setCheckState(Qt::Checked);
                bExists = true;
                break;
            }
        }
        if (bExists)
            continue;
        if (m_pList->count() >= int(FwArc::MAX_TAGS))
        {
            QMessageBox::warning(this, tr("标记过多"),
                                 tr("一个文件最多 %1 个标记。").arg(int(FwArc::MAX_TAGS)));
            break;
        }
        addCandidate(sTag, true);
    }
    m_pEdit->clear();
}

QStringList DlgEntryTags::collectTags(QString *psErr) const
{
    QStringList raw;
    for (int i = 0; i < m_pList->count(); ++i)
    {
        const QListWidgetItem *pItem = m_pList->item(i);
        if (pItem != nullptr && pItem->checkState() == Qt::Checked)
            raw << pItem->text();
    }
    // 输入框里已经打好、但没点「添加」的标记同样生效：用户按「确定」就是确认。
    raw << splitTagInput(m_pEdit->text());
    return FwArc::normalizeTags(raw, psErr);
}

QStringList DlgEntryTags::tags() const
{
    QString sErr;
    return collectTags(&sErr);
}

void DlgEntryTags::accept()
{
    QString sErr;
    collectTags(&sErr);
    if (!sErr.isEmpty())
    {
        QMessageBox::warning(this, tr("标记无效"), sErr);
        return;
    }
    QDialog::accept();
}

bool DlgEntryTags::edit(QWidget *parent, const QString &sFileName,
                        const QStringList &knownTags, const QStringList &currentTags,
                        QStringList *pTags)
{
    DlgEntryTags dlg(sFileName, knownTags, currentTags, parent);
    if (dlg.exec() != QDialog::Accepted)
        return false;
    if (pTags != nullptr)
        *pTags = dlg.tags();
    return true;
}

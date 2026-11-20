#ifndef DLGENTRYTAGS_H
#define DLGENTRYTAGS_H

#include <QDialog>
#include <QStringList>

class QLineEdit;
class QListWidget;
class QPushButton;

// v4 条目「标记」编辑器。
//
// 交互是「勾选已有标记 + 输入框追加新标记」两段式：
//   * 候选来自当前归档里出现过的全部标记（FileManager::allTags()），已勾选的是
//     该条目当前标记；
//   * 输入框用逗号/分号/顿号分隔，回车或「添加」把新标记加入列表并勾选。
// 归一化与校验统一走 FwArc::normalizeTags（与写入端、解析端同一份规则），
// 本类只负责交互与展示，不自己定义一套标记规则。
//
// 构造函数公开是为了测试能直接操作控件而不弹模态框；正常路径用静态 edit()。
class DlgEntryTags : public QDialog
{
    Q_OBJECT

public:
    explicit DlgEntryTags(const QString &sFileName, const QStringList &knownTags,
                          const QStringList &currentTags, QWidget *parent = nullptr);

    // 弹出对话框编辑标记；返回 true 时 *pTags 为归一化后的结果。
    static bool edit(QWidget *parent, const QString &sFileName,
                     const QStringList &knownTags, const QStringList &currentTags,
                     QStringList *pTags);

    // 当前勾选（含输入框里尚未点「添加」的内容），已归一化；输入非法时返回空表。
    QStringList tags() const;

protected:
    void accept() override;

private:
    void addTypedTags();
    QStringList collectTags(QString *psErr) const;
    void addCandidate(const QString &sTag, bool bChecked);

    QListWidget *m_pList = nullptr;
    QLineEdit   *m_pEdit = nullptr;
    QPushButton *m_pAdd  = nullptr;
};

#endif // DLGENTRYTAGS_H

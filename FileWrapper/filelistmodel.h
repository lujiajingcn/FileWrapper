#ifndef FILELISTMODEL_H
#define FILELISTMODEL_H

#include <QStandardItemModel>
#include <QList>
#include <QVector>
#include <algorithm>

// 文件列表模型：排序语义
//   * 平铺模式：所有行按所选列排序；"大小"列按数值字节排序，其余列按显示文本。
//   * 树形模式：**同级排序、目录优先**——
//       - 目录永远排在文件前面（不随升/降序反转）；
//       - 目录之间、文件之间各自按所选列排序：
//           路径 = 名称文本；大小 = 子树合计字节；类型 = 显示文本（目录为"文件夹"）；
//       - 递归应用到每一级子目录，树结构不会被拍平。
//
// 为什么需要它：QStandardItemModel 默认按 DisplayRole 文本排序，
// "99.16 KB" 会排在 "989 B" 前面；而且默认排序不区分目录/文件。
class FileListModel : public QStandardItemModel
{
public:
    enum ItemRole
    {
        BytesRole = Qt::UserRole,       // 文件：自身字节数；目录：子树合计字节
        InfoRole = Qt::UserRole + 1,    // FILEINFO（显式；Qt 的 data()/setData() 默认角色就是它）
        IsDirRole = Qt::UserRole + 2,   // true 表示目录节点
    };

    explicit FileListModel(QObject *parent = nullptr) : QStandardItemModel(parent) {}

    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override
    {
        if (column < 0)
            return;
        sortLevel(invisibleRootItem(), qMax(columnCount(), 1), column, order);
    }

private:
    static bool isDirRow(const QList<QStandardItem *> &row)
    {
        return !row.isEmpty() && row.at(0) != nullptr
               && row.at(0)->data(IsDirRole).toBool();
    }

    static qint64 bytesOfRow(const QList<QStandardItem *> &row)
    {
        const QStandardItem *pSize = row.size() > 1 ? row.at(1) : nullptr;
        return pSize != nullptr ? pSize->data(BytesRole).toLongLong() : -1;
    }

    static QString textOfRow(const QList<QStandardItem *> &row, int column)
    {
        const QStandardItem *pItem = column < row.size() ? row.at(column) : nullptr;
        return pItem != nullptr ? pItem->text() : QString();
    }

    static QString nameOfRow(const QList<QStandardItem *> &row)
    {
        return textOfRow(row, 0);
    }

    static void sortLevel(QStandardItem *pParent, int nCols, int column, Qt::SortOrder order)
    {
        if (pParent == nullptr)
            return;

        const int nRows = pParent->rowCount();
        if (nRows > 1)
        {
            // 用 takeRow 移动原行（不 clone），保留 QStandardItem 的子树与所有 data；
            // 视图的展开状态由调用方（MainWindow）在排序前后捕获/恢复。
            QVector<QList<QStandardItem *>> rows;
            rows.reserve(nRows);
            for (int r = nRows - 1; r >= 0; --r)
                rows.prepend(pParent->takeRow(r));

            std::stable_sort(rows.begin(), rows.end(),
                [column, order](const QList<QStandardItem *> &a, const QList<QStandardItem *> &b) {
                    // 目录优先：与升/降序无关，目录永远在文件前
                    const bool bDirA = isDirRow(a);
                    const bool bDirB = isDirRow(b);
                    if (bDirA != bDirB)
                        return bDirA;

                    int nCmp = 0;
                    if (column == 1)
                    {
                        const qint64 x = bytesOfRow(a);
                        const qint64 y = bytesOfRow(b);
                        nCmp = (x < y) ? -1 : ((x > y) ? 1 : 0);
                    }
                    else
                    {
                        nCmp = QString::compare(textOfRow(a, column),
                                                textOfRow(b, column),
                                                Qt::CaseInsensitive);
                    }
                    if (nCmp == 0)
                        nCmp = QString::compare(nameOfRow(a), nameOfRow(b),
                                                Qt::CaseInsensitive);
                    if (nCmp == 0)
                        return false;
                    return order == Qt::AscendingOrder ? nCmp < 0 : nCmp > 0;
                });

            for (const QList<QStandardItem *> &row : rows)
                pParent->appendRow(row);
        }

        // 递归：每一级目录内部同样排序
        for (int r = 0; r < pParent->rowCount(); ++r)
        {
            if (QStandardItem *pChild = pParent->child(r, 0))
                sortLevel(pChild, nCols, column, order);
        }
    }
};

#endif // FILELISTMODEL_H

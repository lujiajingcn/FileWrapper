#ifndef FILELISTMODEL_H
#define FILELISTMODEL_H

#include <QStandardItemModel>
#include <QList>
#include <QVector>
#include <algorithm>

// 文件列表模型：第 1 列（"大小"）按数值字节排序，其余列仍按文本排序。
//
// 为什么需要它：QStandardItemModel 默认按 DisplayRole 文本排序，
// 大小列显示的是 "99.16 KB"/"989 B" 这类格式化字符串，
// 文本序会把 "99.16 KB" 排到 "989 B" 前面（用户看到的"按大小排序不正确"）。
// 这里改为读第 1 列 Qt::UserRole 里存的字节数做数值比较；格式化文本只用于显示。
class FileListModel : public QStandardItemModel
{
public:
    explicit FileListModel(QObject *parent = nullptr) : QStandardItemModel(parent) {}

    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override
    {
        if (column != 1)
        {
            QStandardItemModel::sort(column, order);
            return;
        }
        sortNumericChildren(invisibleRootItem(), qMax(columnCount(), 1), order);
    }

private:
    static void sortNumericChildren(QStandardItem *pParent, int nCols, Qt::SortOrder order)
    {
        if (pParent == nullptr)
            return;

        const int nRows = pParent->rowCount();
        if (nRows > 1)
        {
            QVector<QList<QStandardItem *>> rows;
            rows.reserve(nRows);
            for (int r = 0; r < nRows; ++r)
            {
                QList<QStandardItem *> row;
                for (int c = 0; c < nCols; ++c)
                {
                    QStandardItem *pChild = pParent->child(r, c);
                    row << (pChild != nullptr ? pChild->clone() : new QStandardItem());
                }
                rows << row;
            }

            std::stable_sort(rows.begin(), rows.end(),
                [order](const QList<QStandardItem *> &a, const QList<QStandardItem *> &b) {
                    const qint64 x = a.size() > 1 ? a.at(1)->data(Qt::UserRole).toLongLong() : -1;
                    const qint64 y = b.size() > 1 ? b.at(1)->data(Qt::UserRole).toLongLong() : -1;
                    return order == Qt::AscendingOrder ? x < y : x > y;
                });

            pParent->removeRows(0, nRows);
            for (const QList<QStandardItem *> &row : rows)
                pParent->appendRow(row);
        }

        // 递归到子目录：树形模式若开启排序也保持数值口径
        for (int r = 0; r < pParent->rowCount(); ++r)
        {
            if (QStandardItem *pChild = pParent->child(r, 0))
                sortNumericChildren(pChild, nCols, order);
        }
    }
};

#endif // FILELISTMODEL_H

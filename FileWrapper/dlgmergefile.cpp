#include "dlgmergefile.h"
#include "ui_dlgmergefile.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QFileInfo>
#include <QDirIterator>
#include <QShortcut>
#include <QTreeView>
#include <QScreen>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include "archiveformat.h"

#include <algorithm>
#include <functional>

DlgMergeFile::DlgMergeFile(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::DlgMergeFile)
{
    ui->setupUi(this);

    this->setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    // 自适应尺寸：固定 1000×600 在小屏/高缩放下可能超出可用区域（4K@150% 实测 1500×900）
    const QRect avail = screen() ? screen()->availableGeometry() : QRect(0, 0, 1280, 800);
    // 小屏/高缩放下不能再用 qMax(720/420) 兜底，否则可用区 <720 时必然越界。
    int w = qMin(1000, qMax(320, avail.width() - 80));
    int h = qMin(600,  qMax(260, avail.height() - 80));
    if (avail.width()  > 40 && w > avail.width()  - 20) w = qMax(200, avail.width()  - 20);
    if (avail.height() > 40 && h > avail.height() - 20) h = qMax(180, avail.height() - 20);
    resize(w, h);

    m_hModelFilePath = new QStandardItemModel(this);
    m_hModelFilePath->setColumnCount(1);
    m_hModelFilePath->setHeaderData(0, Qt::Horizontal, tr("文件路径"));
    ui->treeView->setModel(m_hModelFilePath);
    // 多选 + 整行选中；按 Delete 键等同于点「删除」
    ui->treeView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ui->treeView->setSelectionBehavior(QAbstractItemView::SelectRows);

    auto *hDeleteShortcut = new QShortcut(QKeySequence::Delete, ui->treeView);
    connect(hDeleteShortcut, &QShortcut::activated, this, &DlgMergeFile::on_btnDelete_clicked);

    // 拖放：文件/文件夹直接拖进列表即可加入（比点两次按钮快）
    ui->treeView->setAcceptDrops(true);
    ui->treeView->viewport()->setAcceptDrops(true);
    ui->treeView->viewport()->installEventFilter(this);
    ui->treeView->setToolTip(tr("可直接把文件或文件夹拖到这里"));
    updateSummary();
}

DlgMergeFile::~DlgMergeFile()
{
    delete ui;
}

QStringList DlgMergeFile::getFilePaths()
{
    QStringList qLFilePaths;
    int nRowCount = m_hModelFilePath->rowCount();
    for(int i = 0; i < nRowCount; i++)
    {
        qLFilePaths << m_hModelFilePath->item(i, 0)->text();
    }
    return qLFilePaths;
}

QString DlgMergeFile::getMergedFilePath()
{
    return ui->leMergedFilePath->text();
}

void DlgMergeFile::addPathItem(const QString &sPath, bool bUpdateSummary)
{
    if (sPath.isEmpty() || !duplicatCheck(sPath))
        return;
    m_hModelFilePath->setItem(m_hModelFilePath->rowCount(), 0,
                              new QStandardItem(sPath));
    if (bUpdateSummary)
        updateSummary();
}

// 摘要：输入项数、展开后的文件数/总大小、其中多少个是已合并的 .dat
void DlgMergeFile::updateSummary()
{
    int nInputs = 0, nFiles = 0, nArchives = 0;
    qint64 nBytes = 0;
    const int nCap = 200000;   // 超大目录防止统计本身卡住界面
    for (int i = 0; i < m_hModelFilePath->rowCount(); ++i)
    {
        const QString sPath = m_hModelFilePath->item(i, 0)->text();
        const QFileInfo fi(sPath);
        ++nInputs;
        if (fi.isDir())
        {
            QDirIterator it(sPath, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext() && nFiles < nCap)
            {
                it.next();
                ++nFiles;
                nBytes += it.fileInfo().size();
            }
        }
        else if (fi.isFile())
        {
            ++nFiles;
            nBytes += fi.size();
            if (sPath.endsWith(QStringLiteral(".dat"), Qt::CaseInsensitive))
                ++nArchives;
        }
    }

    QString sText = tr("待合并的文件路径");
    if (nInputs > 0)
    {
        sText += tr("（%1 个输入，约 %2 个文件，%3%4）")
                     .arg(nInputs)
                     .arg(nFiles >= nCap ? QStringLiteral(">") + QString::number(nCap)
                                         : QString::number(nFiles))
                     .arg(FwArc::formatSize(nBytes))
                     .arg(nArchives > 0
                              ? tr("，其中 %1 个已合并归档").arg(nArchives)
                              : QString());
    }
    ui->label->setText(sText);
}

void DlgMergeFile::addDroppedUrls(const QList<QUrl> &urls)
{
    bool bAny = false;
    for (const QUrl &u : urls)
    {
        if (!u.isLocalFile())
            continue;
        addPathItem(QFileInfo(u.toLocalFile()).absoluteFilePath(), false);
        bAny = true;
    }
    if (bAny)
        updateSummary();
}

bool DlgMergeFile::eventFilter(QObject *obj, QEvent *ev)
{
    if (obj != ui->treeView->viewport() && obj != ui->treeView)
        return QDialog::eventFilter(obj, ev);

    switch (ev->type())
    {
    case QEvent::DragEnter:
    case QEvent::DragMove:
    {
        auto *pDrag = static_cast<QDragMoveEvent *>(ev);
        if (pDrag->mimeData() && pDrag->mimeData()->hasUrls())
        {
            // 只接受复制：若接受 MoveAction，Explorer 会删除源文件，
            // 而本对话框只是记录路径、并没有搬运任何字节。
            pDrag->setDropAction(Qt::CopyAction);
            pDrag->accept();
            return true;
        }
        break;
    }
    case QEvent::Drop:
    {
        auto *pDrop = static_cast<QDropEvent *>(ev);
        if (pDrop->mimeData() && pDrop->mimeData()->hasUrls())
        {
            addDroppedUrls(pDrop->mimeData()->urls());
            pDrop->setDropAction(Qt::CopyAction);
            pDrop->accept();
            return true;
        }
        break;
    }
    default:
        break;
    }
    return QDialog::eventFilter(obj, ev);
}

void DlgMergeFile::on_btnAddDir_clicked()
{
    QString sDir = QFileDialog::getExistingDirectory(this, tr("选择待合并的文件"), "", QFileDialog::DontResolveSymlinks);
    addPathItem(sDir);
}

void DlgMergeFile::on_btnAddFile_clicked()
{
    QStringList qLFilePaths = QFileDialog::getOpenFileNames(this, tr("选择待合并的文件"), "", tr("All Files (*.*)"));
    for (const QString &sFilePath : qLFilePaths)
        addPathItem(sFilePath, false);
    if (!qLFilePaths.isEmpty())
        updateSummary();
}

void DlgMergeFile::on_btnDelete_clicked()
{
    // 删除当前选中的项（支持 Ctrl/Shift 多选）。先收集行号，再倒序删除，
    // 避免删前面的行导致后面的行号错位。
    QModelIndexList selected = ui->treeView->selectionModel()->selectedRows();
    if (selected.isEmpty())
    {
        // 有些平台/交互下 selectedRows() 为空但 selectedIndexes() 不为空（单元格选择）
        selected = ui->treeView->selectionModel()->selectedIndexes();
    }
    if (selected.isEmpty())
    {
        QMessageBox::information(this, tr("提示"), tr("请先在列表中选择要删除的文件（可多选）。"));
        return;
    }

    QList<int> rows;
    for (const QModelIndex &idx : selected)
    {
        if (idx.isValid()) rows << idx.row();
    }
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    for (int row : rows)
        m_hModelFilePath->removeRow(row);
    updateSummary();
}

void DlgMergeFile::on_btnMergedFilePath_clicked()
{
    QString sFilePath = QFileDialog::getSaveFileName(this,
                                                tr("合并后的文件路径"),
                                                "",
                                                tr("Class Files (*.dat);;All Files (*.*)"));
    if(sFilePath.isEmpty())
        return;

    ui->leMergedFilePath->setText(sFilePath);
}

void DlgMergeFile::on_btnOk_clicked()
{
    if(m_hModelFilePath->rowCount() == 0)
    {
        QMessageBox::information(this, "提示", "未选择待合并的文件");
        return;
    }

    const QString sOutput = ui->leMergedFilePath->text();
    if(sOutput.isEmpty())
    {
        QMessageBox::information(this, "提示", "合并后的文件路径为空");
        return;
    }

    // 输出必须是文件路径：QFileInfo::exists() 对目录也为真，
    // 若把目录交给底层，QFile::rename 会把整个目录移走后再写归档。
    const QFileInfo outInfo(sOutput);
    if (outInfo.isDir())
    {
        QMessageBox::warning(this, "提示",
            "输出路径是一个已存在的目录：\n" + sOutput +
            "\n\n请选择一个文件路径（例如 ...\\合并结果.dat），不能直接输出到目录。");
        return;
    }

    // P0-1：输出文件不能是任一输入文件。
    // 目录输入不再递归枚举全部文件（大目录会在点确定后卡很久才出现进度框）：
    // 只要输出路径位于输入目录内，就必然可能覆盖输入，直接拒绝。
    const QString outKey = QDir::cleanPath(QFileInfo(sOutput).absoluteFilePath()).toLower();
    for (int i = 0; i < m_hModelFilePath->rowCount(); ++i)
    {
        const QString sItem = m_hModelFilePath->item(i, 0)->text();
        const QFileInfo info(sItem);
        const QString sItemAbs = QDir::cleanPath(info.absoluteFilePath()).toLower();
        if (info.isDir())
        {
            if (outKey == sItemAbs
                || outKey.startsWith(sItemAbs + QLatin1Char('/')))
            {
                QMessageBox::warning(this, "提示",
                    "输出文件不能位于待合并的输入目录内：\n" + sItem +
                    "\n\n合并会先清空输出文件再读取输入，导致源数据丢失。请选择其它输出路径。");
                return;
            }
        }
        else if (sItemAbs == outKey)
        {
            QMessageBox::warning(this, "提示",
                "输出文件不能是待合并文件之一：\n" + sItem +
                "\n\n合并会先清空输出文件再读取输入，导致源数据丢失。请选择其它输出路径。");
            return;
        }
    }

    if (QFileInfo::exists(sOutput))
    {
        if (QMessageBox::question(this, "提示",
                "输出文件已存在，合并后会替换它：\n" + sOutput + "\n\n是否继续？",
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
    }
    accept();
}

void DlgMergeFile::on_btnCancel_clicked()
{
    reject();
}

bool DlgMergeFile::duplicatCheck(QString sFilePath)
{
    int nRowCount = m_hModelFilePath->rowCount();
    for(int i = 0; i < nRowCount; i++)
    {
        QString sItemText = m_hModelFilePath->item(i, 0)->text();
        if(QString::compare(sItemText, sFilePath) == 0)
            return false;
    }
    return true;
}

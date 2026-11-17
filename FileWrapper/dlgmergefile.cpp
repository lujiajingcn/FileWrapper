#include "dlgmergefile.h"
#include "ui_dlgmergefile.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QFileInfo>
#include <QDirIterator>
#include <QShortcut>
#include <QTreeView>
#include <QMenu>
#include <QToolButton>

#include <algorithm>
#include <functional>

DlgMergeFile::DlgMergeFile(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::DlgMergeFile)
{
    ui->setupUi(this);

    this->setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    resize(1000, 600);

    m_hModelFilePath = new QStandardItemModel;
    m_hModelFilePath->setColumnCount(1);
    m_hModelFilePath->setHeaderData(0, Qt::Horizontal, tr("文件路径"));
    ui->treeView->setModel(m_hModelFilePath);
    // 多选 + 整行选中；按 Delete 键等同于点「删除」
    ui->treeView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ui->treeView->setSelectionBehavior(QAbstractItemView::SelectRows);

    auto *hDeleteShortcut = new QShortcut(QKeySequence::Delete, ui->treeView);
    connect(hDeleteShortcut, &QShortcut::activated, this, &DlgMergeFile::on_btnDelete_clicked);

    // 「添加」一个入口，两种粒度：点击按钮 = 添加文件；点右侧箭头 = 也可以添加文件夹。
    // 两者并不重复：文件是"挑几个具体文件"（可跨目录多选），文件夹是"整个目录递归合并"。
    auto *hAddMenu = new QMenu(this);
    QAction *hActFiles = hAddMenu->addAction(tr("添加文件…"));
    QAction *hActDir   = hAddMenu->addAction(tr("添加文件夹…"));
    ui->btnAdd->setMenu(hAddMenu);
    ui->btnAdd->setPopupMode(QToolButton::MenuButtonPopup);
    connect(hActFiles, &QAction::triggered, this, &DlgMergeFile::addFiles);
    connect(hActDir,   &QAction::triggered, this, &DlgMergeFile::addDirectory);
    connect(ui->btnAdd, &QToolButton::clicked, this, &DlgMergeFile::addFiles);
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

void DlgMergeFile::addPathItem(const QString &sPath)
{
    if (sPath.isEmpty() || !duplicatCheck(sPath))
        return;
    m_hModelFilePath->setItem(m_hModelFilePath->rowCount(), 0,
                              new QStandardItem(sPath));
}

void DlgMergeFile::addFiles()
{
    const QStringList qLFilePaths =
        QFileDialog::getOpenFileNames(this, tr("选择待合并的文件"), "",
                                      tr("All Files (*.*)"));
    for (const QString &sFilePath : qLFilePaths)
        addPathItem(sFilePath);
}

void DlgMergeFile::addDirectory()
{
    const QString sDir = QFileDialog::getExistingDirectory(
        this, tr("选择待合并的文件夹"), "", QFileDialog::DontResolveSymlinks);
    addPathItem(sDir);
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

    // P0-1：输出文件不能是任一输入文件（目录输入需展开后逐一比较）。
    // 底层 FileManager 也会再校验一次，这里提前拦截可给出更明确的提示。
    const QString outKey = QFileInfo(sOutput).absoluteFilePath().toLower();
    for (int i = 0; i < m_hModelFilePath->rowCount(); ++i)
    {
        const QString sItem = m_hModelFilePath->item(i, 0)->text();
        const QFileInfo info(sItem);
        if (info.isDir())
        {
            QDirIterator it(sItem, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext())
            {
                const QString sub = it.next();
                if (QFileInfo(sub).absoluteFilePath().toLower() == outKey)
                {
                    QMessageBox::warning(this, "提示",
                        "输出文件不能是待合并文件之一：\n" + sub +
                        "\n\n合并会先清空输出文件再读取输入，导致源数据丢失。请选择其它输出路径。");
                    return;
                }
            }
        }
        else if (info.absoluteFilePath().toLower() == outKey)
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

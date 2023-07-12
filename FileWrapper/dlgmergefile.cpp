#include "dlgmergefile.h"
#include "ui_dlgmergefile.h"
#include <QFileDialog>
#include <QMessageBox>

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

void DlgMergeFile::on_btnAddDir_clicked()
{
    QString sDir = QFileDialog::getExistingDirectory(this, tr("选择待合并的文件"), "", QFileDialog::DontResolveSymlinks);
    if(sDir.isEmpty() || !duplicatCheck(sDir))
        return;

    QStandardItem* hItem = new QStandardItem(sDir);
    hItem->setCheckable(true);
    m_hModelFilePath->setItem(m_hModelFilePath->rowCount(), 0, hItem);
}

void DlgMergeFile::on_btnAddFile_clicked()
{
    QStringList qLFilePaths = QFileDialog::getOpenFileNames(this, tr("选择待合并的文件"), "", tr("All Files (*.*)"));

    foreach(QString sFilePath, qLFilePaths)
    {
        if(!duplicatCheck(sFilePath))
            continue;

        QStandardItem* hItem = new QStandardItem(sFilePath);
        hItem->setCheckable(true);
        m_hModelFilePath->setItem(m_hModelFilePath->rowCount(), 0, hItem);
    }
}

void DlgMergeFile::on_btnDelete_clicked()
{
    int nRowCount = m_hModelFilePath->rowCount();
    for(int i = nRowCount - 1; i >= 0; i--)
    {
        Qt::CheckState checkState = m_hModelFilePath->item(i, 0)->checkState();
        if(checkState == Qt::Checked)
        {
            m_hModelFilePath->removeRow(i);
        }
    }
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
        QMessageBox().information(nullptr, "", "未选择待合并的文件");
        return;
    }

    if(ui->leMergedFilePath->text().isEmpty())
    {
        QMessageBox().information(nullptr, "提示", "合并后的文件路径为空");
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

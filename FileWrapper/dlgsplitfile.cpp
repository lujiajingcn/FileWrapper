#include "dlgsplitfile.h"
#include "ui_dlgsplitfile.h"
#include <QFileDialog>
#include <QMessageBox>

DlgSplitFile::DlgSplitFile(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::DlgSplitFile)
{
    ui->setupUi(this);

    resize(1000, size().height());

    this->setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
}

DlgSplitFile::~DlgSplitFile()
{
    delete ui;
}

QString DlgSplitFile::getMergedFilePath()
{
    return ui->leMergedFilePath->text();
}

bool DlgSplitFile::getIsSaveAsOldPath()
{
    return ui->rbSaveAsOldPath->isChecked();
}

QString DlgSplitFile::getSplitFileDir()
{
    return ui->leSplittedFileDir->text();
}

void DlgSplitFile::on_btnMergedFilePath_clicked()
{
    QString sFilePath = QFileDialog::getOpenFileName(this, tr("待分割的文件"), "", tr("Class Files (*.dat);;All Files (*.*)"));
    if(sFilePath.isEmpty())
        return;

    ui->leMergedFilePath->setText(sFilePath);
}

void DlgSplitFile::on_rbSaveAsOldPath_clicked(bool checked)
{
    ui->leSplittedFileDir->setEnabled(!checked);
    ui->btnSplittedFileDir->setEnabled(!checked);
}

void DlgSplitFile::on_brSaveAsSelectedDir_clicked(bool checked)
{
    ui->leSplittedFileDir->setEnabled(checked);
    ui->btnSplittedFileDir->setEnabled(checked);
}

void DlgSplitFile::on_btnSplittedFileDir_clicked()
{
    QString sDir = QFileDialog::getExistingDirectory(this, "", "", QFileDialog::DontResolveSymlinks);
    if(sDir.isEmpty())
        return;
    ui->leSplittedFileDir->setText(sDir);
}


void DlgSplitFile::on_btnOk_clicked()
{
    if(ui->leMergedFilePath->text().isEmpty())
    {
        QMessageBox().information(nullptr, "", "未选择待分割的文件");
        return;
    }
    if(ui->brSaveAsSelectedDir->isChecked() && ui->leSplittedFileDir->text().isEmpty())
    {
        QMessageBox().information(nullptr, "", "按照选择的目录存放分割后的文件时 目录不能为空");
        return;
    }

    accept();
}

void DlgSplitFile::on_btnCancel_clicked()
{
    reject();
}

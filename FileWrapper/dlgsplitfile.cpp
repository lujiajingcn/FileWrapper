#include "dlgsplitfile.h"
#include "ui_dlgsplitfile.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QScreen>
#include "icons.h"
#include "archiveformat.h"

DlgSplitFile::DlgSplitFile(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::DlgSplitFile)
{
    ui->setupUi(this);
    ui->btnMergedFilePath->setIcon(FwIcons::get(QStringLiteral("browse")));
    ui->btnSplittedFileDir->setIcon(FwIcons::get(QStringLiteral("browse")));
    ui->btnOk->setIcon(FwIcons::get(QStringLiteral("ok")));
    ui->btnCancel->setIcon(FwIcons::get(QStringLiteral("cancel")));

    // 固定 1000px 在 1366@150%/1920@200% 下会超出可用区，右侧按钮被裁。
    int w = 1000;
    if (screen()) {
        const QRect avail = screen()->availableGeometry();
        if (avail.width() > 40 && w > avail.width() - 20)
            w = qMax(480, avail.width() - 20);
    }
    resize(w, size().height());

    this->setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    connect(ui->leMergedFilePath, &QLineEdit::textChanged, this,
            [this](const QString &) { updateButtonStates(); });
    connect(ui->leSplittedFileDir, &QLineEdit::textChanged, this,
            [this](const QString &) { updateButtonStates(); });
    connect(ui->rbSaveAsOldPath, &QRadioButton::toggled, this,
            [this](bool) { updateButtonStates(); });
    connect(ui->brSaveAsSelectedDir, &QRadioButton::toggled, this,
            [this](bool) { updateButtonStates(); });
    updateButtonStates();
}

void DlgSplitFile::updateButtonStates()
{
    const bool bHasInput = !ui->leMergedFilePath->text().trimmed().isEmpty();
    const bool bHasDir = !ui->leSplittedFileDir->text().trimmed().isEmpty();
    const bool bOk = bHasInput
                     && (ui->rbSaveAsOldPath->isChecked() || bHasDir);
    ui->btnOk->setEnabled(bOk);
    const bool bSelectDir = ui->brSaveAsSelectedDir->isChecked();
    ui->leSplittedFileDir->setEnabled(bSelectDir);
    ui->btnSplittedFileDir->setEnabled(bSelectDir);
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

void DlgSplitFile::setMergedFilePath(const QString &sPath)
{
    ui->leMergedFilePath->setText(sPath);
    updateButtonStates();
}

void DlgSplitFile::on_btnMergedFilePath_clicked()
{
    QString sFilePath = QFileDialog::getOpenFileName(this, tr("待分割的文件"), "", FwArc::fileDialogFilter());
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
        QMessageBox::information(this, "提示", "未选择待分割的文件");
        return;
    }
    if(ui->brSaveAsSelectedDir->isChecked() && ui->leSplittedFileDir->text().isEmpty())
    {
        QMessageBox::information(this, "提示", "按照选择的目录存放分割后的文件时 目录不能为空");
        return;
    }

    accept();
}

void DlgSplitFile::on_btnCancel_clicked()
{
    reject();
}

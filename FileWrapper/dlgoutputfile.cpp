#include "dlgoutputfile.h"
#include "ui_dlgoutputfile.h"
#include <QFileDialog>

DlgOutputFile::DlgOutputFile(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::DlgOutputFile)
{
    ui->setupUi(this);

    this->setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
}

DlgOutputFile::~DlgOutputFile()
{
    delete ui;
}

QString DlgOutputFile::getOutputDir()
{
    return m_sOutputDir;
}

void DlgOutputFile::on_btnSelectDir_clicked()
{
    QString sDir = QFileDialog::getExistingDirectory(this, "", "", QFileDialog::DontResolveSymlinks);
    if(sDir.isEmpty())
        return;
    ui->leOutputFileDir->setText(sDir);
}

void DlgOutputFile::on_rbOldPath_clicked()
{
    ui->leOutputFileDir->setEnabled(false);
    ui->btnSelectDir->setEnabled(false);
}

void DlgOutputFile::on_rbSelectDir_clicked()
{
    ui->leOutputFileDir->setEnabled(true);
    ui->btnSelectDir->setEnabled(true);
}

void DlgOutputFile::on_btnOk_clicked()
{
    m_sOutputDir = ui->leOutputFileDir->text();
    accept();
}

void DlgOutputFile::on_btnCancel_clicked()
{
    reject();
}

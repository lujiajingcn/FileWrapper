#include "dlgoutputfile.h"
#include "ui_dlgoutputfile.h"
#include <QFileDialog>
#include <QMessageBox>

DlgOutputFile::DlgOutputFile(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::DlgOutputFile)
{
    ui->setupUi(this);

    this->setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    // 按 .ui 的初始选中项同步启用状态（默认"原路径导出"）
    if (ui->rbOldPath->isChecked())
        on_rbOldPath_clicked();
    else
        on_rbSelectDir_clicked();
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
    // 切回"原路径导出"必须清掉之前选择的目录文本，否则确定时仍会按旧目录导出
    ui->leOutputFileDir->clear();
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
    // 以单选按钮状态决定导出方式；"原路径导出"时输出目录必须为空
    if (ui->rbOldPath->isChecked())
    {
        m_sOutputDir.clear();
    }
    else
    {
        m_sOutputDir = ui->leOutputFileDir->text().trimmed();
        if (m_sOutputDir.isEmpty())
        {
            QMessageBox::information(this, tr("选择导出路径"),
                                     tr("请先选择导出目录，或改选“原路径导出”。"));
            return;
        }
    }
    accept();
}

void DlgOutputFile::on_btnCancel_clicked()
{
    reject();
}

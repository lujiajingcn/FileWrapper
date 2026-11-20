#include "dlgoutputfile.h"
#include "ui_dlgoutputfile.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include "icons.h"

DlgOutputFile::DlgOutputFile(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::DlgOutputFile)
{
    ui->setupUi(this);
    ui->btnSelectDir->setIcon(FwIcons::get(QStringLiteral("browse")));
    // 确定/取消是对话框的默认按钮（主题里 :default 为实心蓝底），固定深色图标
    // 会和蓝底糊在一起；按钮文字已经足够清楚，不给默认按钮加图标。

    this->setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    // .ui 原本全是绝对坐标，系统字体放大后标签/按钮会被裁切。
    // 这里按控件重建布局，保持原有控件与信号连接。
    auto *pMain = new QVBoxLayout(this);
    auto *pRadios = new QHBoxLayout;
    pRadios->addWidget(ui->rbOldPath);
    pRadios->addWidget(ui->rbSelectDir);
    pRadios->addStretch(1);
    pMain->addLayout(pRadios);

    auto *pDir = new QHBoxLayout;
    pDir->addWidget(ui->label);
    pDir->addWidget(ui->leOutputFileDir, 1);
    pDir->addWidget(ui->btnSelectDir);
    pMain->addLayout(pDir);

    auto *pButtons = new QHBoxLayout;
    pButtons->addStretch(1);
    pButtons->addWidget(ui->btnOk);
    pButtons->addWidget(ui->btnCancel);
    pMain->addLayout(pButtons);
    setMinimumWidth(520);

    // 按 .ui 的初始选中项同步启用状态（默认"原路径导出"）
    if (ui->rbOldPath->isChecked())
        on_rbOldPath_clicked();
    else
        on_rbSelectDir_clicked();
    connect(ui->leOutputFileDir, &QLineEdit::textChanged, this,
            [this](const QString &) { updateButtonStates(); });
    connect(ui->rbOldPath, &QRadioButton::toggled, this,
            [this](bool) { updateButtonStates(); });
    connect(ui->rbSelectDir, &QRadioButton::toggled, this,
            [this](bool) { updateButtonStates(); });
    updateButtonStates();
}

void DlgOutputFile::updateButtonStates()
{
    const bool bOld = ui->rbOldPath->isChecked();
    const bool bHasDir = !ui->leOutputFileDir->text().trimmed().isEmpty();
    ui->btnOk->setEnabled(bOld || bHasDir);
    ui->btnSelectDir->setEnabled(!bOld);
    ui->leOutputFileDir->setEnabled(!bOld);
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
    updateButtonStates();
}

void DlgOutputFile::on_rbSelectDir_clicked()
{
    ui->leOutputFileDir->setEnabled(true);
    ui->btnSelectDir->setEnabled(true);
    updateButtonStates();
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

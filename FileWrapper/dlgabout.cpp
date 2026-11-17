#include "dlgabout.h"
#include "ui_dlgabout.h"

DlgAbout::DlgAbout(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::DlgAbout)
{
    ui->setupUi(this);

    this->setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
}

void DlgAbout::setExtraInfo(const QString &sHtml)
{
    if (!sHtml.isEmpty())
        ui->textEdit->append(sHtml);
}

DlgAbout::~DlgAbout()
{
    delete ui;
}

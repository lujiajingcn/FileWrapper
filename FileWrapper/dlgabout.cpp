#include "dlgabout.h"
#include "ui_dlgabout.h"

DlgAbout::DlgAbout(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::DlgAbout)
{
    ui->setupUi(this);

    this->setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
}

DlgAbout::~DlgAbout()
{
    delete ui;
}

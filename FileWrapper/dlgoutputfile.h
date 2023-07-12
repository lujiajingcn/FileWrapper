#ifndef DLGOUTPUTFILE_H
#define DLGOUTPUTFILE_H

#include <QDialog>

namespace Ui {
class DlgOutputFile;
}

class DlgOutputFile : public QDialog
{
    Q_OBJECT

public:
    explicit DlgOutputFile(QWidget *parent = nullptr);
    ~DlgOutputFile();

    QString getOutputDir();
private slots:
    void on_btnSelectDir_clicked();

    void on_rbOldPath_clicked();

    void on_rbSelectDir_clicked();

    void on_btnOk_clicked();

    void on_btnCancel_clicked();

private:
    QString             m_sOutputDir;
    Ui::DlgOutputFile *ui;
};

#endif // DLGOUTPUTFILE_H

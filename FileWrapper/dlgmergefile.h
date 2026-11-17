#ifndef DLGMERGEFILE_H
#define DLGMERGEFILE_H

#include <QDialog>
#include <QStandardItemModel>

namespace Ui {
class DlgMergeFile;
}

class DlgMergeFile : public QDialog
{
    Q_OBJECT

public:
    explicit DlgMergeFile(QWidget *parent = nullptr);
    ~DlgMergeFile();

    QStringList getFilePaths();
    QString getMergedFilePath();

private slots:
    void on_btnAddDir_clicked();

    void on_btnAddFile_clicked();

    void on_btnDelete_clicked();

    void on_btnMergedFilePath_clicked();

    void on_btnOk_clicked();

    void on_btnCancel_clicked();

protected:
    bool duplicatCheck(QString sFilePath);

private:
    QStandardItemModel      *m_hModelFilePath;
    Ui::DlgMergeFile *ui;
};

#endif // DLGMERGEFILE_H

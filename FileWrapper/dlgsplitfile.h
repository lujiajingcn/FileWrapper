#ifndef DLGSPLITFILE_H
#define DLGSPLITFILE_H

#include <QDialog>

namespace Ui {
class DlgSplitFile;
}

class DlgSplitFile : public QDialog
{
    Q_OBJECT

public:
    explicit DlgSplitFile(QWidget *parent = nullptr);
    ~DlgSplitFile();

    QString getMergedFilePath();
    bool getIsSaveAsOldPath();
    QString getSplitFileDir();

private slots:
    void on_btnMergedFilePath_clicked();

    void on_rbSaveAsOldPath_clicked(bool checked);

    void on_brSaveAsSelectedDir_clicked(bool checked);

    void on_btnSplittedFileDir_clicked();

    void on_btnOk_clicked();

    void on_btnCancel_clicked();

private:
    Ui::DlgSplitFile *ui;
};

#endif // DLGSPLITFILE_H

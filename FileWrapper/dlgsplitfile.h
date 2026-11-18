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

    // 预填"待分割文件路径"（已加载归档时由主窗口传入，避免用户再手选一次）
    void setMergedFilePath(const QString &sPath);

private slots:
    void on_btnMergedFilePath_clicked();

    void on_rbSaveAsOldPath_clicked(bool checked);

    void on_brSaveAsSelectedDir_clicked(bool checked);

    void on_btnSplittedFileDir_clicked();

    void on_btnOk_clicked();

    void on_btnCancel_clicked();

private:
    void updateButtonStates();
    Ui::DlgSplitFile *ui;
};

#endif // DLGSPLITFILE_H

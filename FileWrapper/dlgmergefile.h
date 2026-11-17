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
    void on_btnDelete_clicked();

    void on_btnMergedFilePath_clicked();

    void on_btnOk_clicked();

    void on_btnCancel_clicked();

protected:
    bool duplicatCheck(QString sFilePath);

private:
    void addFiles();                  // 添加文件…（可多选）
    void addDirectory();              // 添加文件夹…（递归合并整个目录）
    void addPathItem(const QString &sPath);   // 去重后追加一行

    QStandardItemModel      *m_hModelFilePath;
    Ui::DlgMergeFile *ui;
};

#endif // DLGMERGEFILE_H

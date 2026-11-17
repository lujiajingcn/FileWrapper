#ifndef DLGMERGEFILE_H
#define DLGMERGEFILE_H

#include <QDialog>
#include <QStandardItemModel>
#include <QList>
#include <QUrl>

class QEvent;

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
    // 支持把文件/文件夹直接拖进列表
    bool eventFilter(QObject *obj, QEvent *ev) override;

private:
    void addPathItem(const QString &sPath);
    void updateSummary();                        // 刷新"共 N 个文件 / X MB"摘要
    void addDroppedUrls(const QList<QUrl> &urls);

    QStandardItemModel      *m_hModelFilePath;
    Ui::DlgMergeFile *ui;
};

#endif // DLGMERGEFILE_H

#ifndef DLGMERGEFILE_H
#define DLGMERGEFILE_H

#include <QDialog>
#include <QStandardItemModel>
#include <QList>
#include <QUrl>

class QEvent;
class QLabel;
class QResizeEvent;

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

    // 输出路径没有扩展名时补 .dat（已有扩展名——含 "v1.2" 这类——原样返回）。
    // 单独抽成静态函数：对话框要问用户、测试要能直接断言，不依赖模态弹窗。
    static QString ensureArchiveSuffix(const QString &sPath);

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
    // 窗口宽度变化后按当前宽度重算路径提示条的最小高度，避免长路径末行被裁掉。
    void resizeEvent(QResizeEvent *pEvent) override;

private:
    void addPathItem(const QString &sPath, bool bUpdateSummary = true);
    void updateSummary();                        // 刷新"共 N 个文件 / X MB"摘要
    void updateButtonStates();                   // 无文件/无选中/无输出时禁用对应按钮
    // 把输出路径解析成绝对路径并实时显示在输入框下方：
    // 相对路径（比如只填 "a"）到底写到哪，必须输入完就能看到，而不是合并完去猜。
    void updateResolvedPath();
    void adjustResolvedPathHeight();
    // 输出路径的硬性错误（目录 / 以分隔符结尾 / 与输入冲突）；空串表示可用。
    // 实时提示和「确定」校验共用这一份规则，避免两处判断漂移。
    QString outputPathProblem(const QString &sPath) const;
    // 空列表时把"可拖到这里"的提示直接画在树区域中央（不再只靠 tooltip）
    void updateDropHint();
    void addDroppedUrls(const QList<QUrl> &urls);

    QStandardItemModel      *m_hModelFilePath;
    QLabel                  *m_lblDropHint = nullptr;
    bool                     m_bAdjustingPathHeight = false;   // 防止 setMinimumHeight 递归
    bool                     m_bPathHeightPending = false;     // 合并一次 resize 的延迟重算
    Ui::DlgMergeFile *ui;
};

#endif // DLGMERGEFILE_H

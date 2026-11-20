#ifndef DLGMERGEFILE_H
#define DLGMERGEFILE_H

#include <QDialog>
#include <QSharedPointer>
#include <QStandardItemModel>
#include <QList>
#include <QUrl>
#include <QAtomicInt>

class QEvent;
class QLabel;
class QResizeEvent;
class QThreadPool;
class QTimer;

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

    // 输出路径没有扩展名时补 .fwda（已有扩展名——含 "v1.2" 这类——原样返回）。
    // 单独抽成静态函数：对话框要问用户、测试要能直接断言，不依赖模态弹窗。
    static QString ensureArchiveSuffix(const QString &sPath);

    // 输入路径展开后的统计结果（输入项数 / 递归文件数 / 总字节数 / 归档个数）。
    // 目录遍历可能几秒（几十万文件），因此计算在工作线程里做，UI 只负责显示结果；
    // 纯函数、不碰界面，测试可以直接调用。
    struct MergeSummary
    {
        int    nInputs   = 0;
        int    nFiles    = 0;
        bool   bCapped   = false;   // 文件数触顶（>20 万）：后面的不再数
        qint64 nBytes    = 0;
        int    nArchives = 0;
    };
    static constexpr int kSummaryFileCap = 200000;   // 统计上限，避免扫描本身卡住
    // pAbort 非空时定期检查：置位后立即返回（对话框关闭 / 又有新的输入要统计）。
    static MergeSummary computeMergeSummary(const QStringList &paths,
                                            QAtomicInt *pAbort = nullptr);

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
    // 输入列表变化后重新统计摘要：作废旧一轮、防抖后交给线程池扫描，
    // 不在这里遍历目录（同步扫描会让「添加文件夹」卡住，列表迟迟不刷新）。
    void updateSummary();
    void startSummaryScan();                     // 把当前输入快照交给线程池
    void applySummary(const MergeSummary &summary, quint64 nGeneration);
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
    // 摘要统计的异步管线：一次只跑一轮，新输入/关闭对话框会作废旧的一轮。
    QThreadPool             *m_summaryPool = nullptr;
    QTimer                  *m_summaryDebounce = nullptr;   // 连续添加时合并成一次扫描
    QTimer                  *m_summarySlowHint = nullptr;   // 扫描久了才显示「正在统计…」
    QSharedPointer<QAtomicInt> m_summaryAbort;              // 非空且为 1 = 当前一轮作废
    quint64                  m_summaryGeneration = 0;
    bool                     m_bSummaryRunning = false;
    bool                     m_bAdjustingPathHeight = false;   // 防止 setMinimumHeight 递归
    bool                     m_bPathHeightPending = false;     // 合并一次 resize 的延迟重算
    Ui::DlgMergeFile *ui;
};

#endif // DLGMERGEFILE_H

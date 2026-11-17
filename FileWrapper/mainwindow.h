#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QStandardItemModel>
#include "filemanager.h"
#include "plugininterface.h"
#include <QTreeView>
#include <QStackedWidget>
#include <QHeaderView>
#include <QToolBar>
#include <QAction>
#include <QMenu>
#include <QPoint>
#include <QToolButton>
#include <QSet>
#include <QHash>
#include <QStringList>
#include <QVector>
#include <QMetaObject>
#include <QElapsedTimer>

class QProgressDialog;
class QCloseEvent;

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    void initWidgetFilePath();
    void initWidgetFileContent();
    // 关闭窗口前确认未保存改动（P0：此前直接退出会静默丢失改动）
    void closeEvent(QCloseEvent *event) override;

signals:
    void QMergeFiles(QVector<QString> vtInputFiles, QString sOutputFile);
    void QSplitFiles(QString sInputFile, bool bIsSaveAsOldPath, QString sSplitFileDir);

private slots:
    void on_treeView_doubleClicked(const QModelIndex &index);
    void on_actionLoadFile_triggered();
    void on_actionMergeFile_triggered();
    void on_actionSplitFile_triggered();
    void onActionViewTreeTriggered();
    void onActionViewFlatTriggered();
    void onActionShowFilePathTriggered();
    void onActionShowFileNameTriggered();
    void onActionAddFileTriggered();
    void onActionDelFileTriggered();
    void onActionOutputFileTriggered();
    void onActionSaveFileTriggered();
    void onActionVerifyArchiveTriggered();
    void onFileManagerWarning(const QString &sMsg);
    void on_actionAbout_triggered();
    void on_actionPluginMap_triggered();
    void onActionPluginDiagnosticsTriggered();
    void onActionPluginManagerTriggered();
    void onActionReloadPluginsTriggered();
    void onActionSupportedFormatsTriggered();
    void on_actionUnloadFile_triggered();

    void onTabCloseRequested(int index);
    void onTabCurrentChanged(int index);
    void onTreeViewCustomContextMenu(const QPoint &pos);
    void onActionNewTabTriggered();

protected:
    bool FindFile(const QString sDir, QStringList& arrFileExts, QStringList& arrFiles, bool bSubDir);
    bool CheckFileExt(QString sFileName, QStringList& arrFileExts);
    QString choosePlugin(QString sFileExt);
    // 事件过滤器：接管文件列表的鼠标双击（绕开 QAbstractItemView 对首次双击的丢弃）
    bool eventFilter(QObject *obj, QEvent *event) override;
    void populateFilePathView(bool bShowPath);
    void refreshFilePathView();
    void populateFilePathList(bool bShowPath);
    void populateFilePathTree();
    void updateViewActionState();

    // 统一刷新所有工具栏 / 菜单动作（含保存按钮），消除「列表空了但保存可用」的状态错乱
    void syncActionStates();
    void setOperationsEnabled(bool bEnabled);

    void collectExpandedItems(const QModelIndex &index, const QString &sKeyPrefix, QSet<QString> &setExpanded);
    void restoreExpandedItems(const QModelIndex &index, const QString &sKeyPrefix, const QSet<QString> &setExpanded);
    // 取消 / 保存
    bool confirmDiscardOrSaveIfDirty();
    // 校验并加载归档（加载文件与"打开合并结果"共用；失败时保持当前归档不变）
    void loadArchiveFile(const QString &sFileName);
    // 文件列表筛选（按路径/文件名，大小写不敏感）
    bool matchesFilter(const FILEINFO &info) const;
    static QString typeLabel(const QString &sFileName);
    void saveUiState();
    void restoreUiState();
    // 文件列表双击的统一处理（目录 -> 展开/收起，文件 -> 打开）
    void handleFileListDoubleClick(const QModelIndex &index);

    // 长操作的进度对话框连接管理（避免每次 disconnect 掉其它常驻连接）
    void beginProgressOperation(QProgressDialog *dlg, bool *pbOk, QString *psResultMsg);
    void endProgressOperation();

    // 大文件：流式抽取到临时文件后把路径交给插件，避免全量内存
    // pbNoSpace 输出"失败原因是磁盘空间不足"（此时不能退回内存通道）
    bool extractToTempFile(const FILEINFO &info, QString &sOutPath, QString &sErr,
                           bool *pbNoSpace = nullptr);
    void cleanupTempFiles();
    void discardTempCache();

private:
    struct TabDoc {
        bool             hasFile = false;
        FILEINFO         info;
        QString          pluginPath;
        PluginInterface *interface = nullptr;
        bool             ownsInterface = false;
        bool             needsRender = true;
    };

    QStandardItemModel      *m_hModelFilePath;
    QLineEdit               *m_leFilter = nullptr;
    QString                  m_sFilter;
    FileManager             *m_fileManager;

    QStackedWidget          *m_swShowArea;
    QTreeView               *m_tvFilePath;
    QToolBar                *m_tbFilePath;
    QAction                 *m_acViewTree;
    QAction                 *m_acViewFlat;
    QAction                 *m_acShowFilePath;
    QAction                 *m_acShowFileName;
    QAction                 *m_acAddFile;
    QAction                 *m_acDelFile;
    QAction                 *m_acOutputFile;
    QAction                 *m_acSaveFile;

    bool                    m_bTreeMode = true;
    bool                    m_bShowPath = false;

    PluginInterface         *m_pCurrentInterface = nullptr;

    QVector<TabDoc>          m_vTabDocs;
    QSet<QWidget*>           m_setPluginWidgets;
    // 已经提示过「插件不可用」且用户选择放弃的插件：避免对同一插件反复弹窗
    QSet<QString>            m_setWarnedPlugins;
    QSet<QString>            m_setTempFiles;
    QHash<QString, QString>  m_mapTempCache;    // 条目标识 -> 临时文件，重复打开时复用
    QStringList              m_listTempOrder;   // 创建顺序，用于上限淘汰
    quint64                  m_uTempSerial = 0; // 单调递增，保证临时文件名不重复

    QMetaObject::Connection  m_connProgress;
    QMetaObject::Connection  m_connFinished;
    QMetaObject::Connection  m_connCancel;
    QProgressDialog         *m_pActiveProgress = nullptr;
    QElapsedTimer            m_progressTimer;

    void openFileInNewTab(const FILEINFO& info);
    void openFileInCurrentTab(const FILEINFO& info);
    void activateTab(int index);
    void renderFile(const FILEINFO& info, PluginInterface* iface);
    void destroyTabAt(int index);
    void stopCurrentPlaybackAndInvalidate();
    void closeAllTabs();
    void newEmptyTab();
    bool resolvePlugin(const FILEINFO &info, QString &sPluginPath, PluginInterface* &pInterface, bool &bOwned);

    // 插件不可用时的提示与兜底：显示可读原因 + 部署自检结果，允许「重试加载」或
    // 「以文本查看」。返回可用模板实例；nullptr 表示用户放弃（此时 *pbUseBuiltinText
    // 为 true 表示应改用内置文本查看器）。
    PluginInterface *promptPluginUnavailable(const QString &sPluginPath, const QString &sFileName,
                                             bool *pbUseBuiltinText);
    // 「插件设置 → 插件诊断」：列出已加载插件、加载失败原因与缺失的部署依赖
    void showPluginDiagnostics();
    // 「插件设置 → 支持的格式」：已安装插件覆盖的后缀 + 常见不支持格式的建议
    void showSupportedFormats();
    // 「插件设置 → 插件管理」：勾选启用/禁用插件（C5）
    void showPluginManagerDialog();
    // 释放所有标签页后让 PluginManager 重新扫描 plugins/（C5）
    void reloadAllPlugins();

    Ui::MainWindow *ui;
};

#endif // MAINWINDOW_H

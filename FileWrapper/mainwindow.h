#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QStandardItemModel>
#include "filemanager.h"
#include "plugininterface.h"
#include "formatresolver.h"    // RouteDecision 内嵌 Detection
#include "dlgfileproperty.h"   // 「属性」数据收集的返回值类型
#include <QTreeView>
#include <QListView>
#include <QStackedWidget>
#include <QHeaderView>
#include <QPixmap>
#include <QImage>
#include <QPersistentModelIndex>
#include <QToolBar>
#include <QLayout>
#include <QAction>
#include <QMenu>
#include <QPoint>
#include <QToolButton>
#include <QTabBar>
#include <QIcon>
#include <QSet>
#include <QHash>
#include <QStringList>
#include <QVector>
#include <QMetaObject>
#include <QElapsedTimer>
#include <QTimer>
#include <QSharedPointer>
#include <QPointer>
#include <QMutex>
#include <QWaitCondition>
#include <atomic>

class QProgressDialog;
class QCloseEvent;
class QResizeEvent;
class QThreadPool;

// 缩略图后台解码的共享门：任务线程在完成时归还 inFlight，主线程在保存前
// 等待它归零（Windows 下后台仍打开归档句柄会让 rename 失败）。
struct ThumbnailGate
{
    QMutex mutex;
    QWaitCondition idle;
    int inFlight = 0;
    std::atomic<quint64> generation{0};   // 切归档/切视图后旧结果直接丢弃
};

namespace Ui {
class MainWindow;
}

// 会换行的"流式"布局：子项按可用宽度从左到右排列，排满自动换到下一行。
// 用途：左侧文件列表的图标工具条——QToolBar 在窄栏里会把放不下的动作折进
// 溢出菜单（"»"），按钮直接消失；换行后 8 个按钮在小窗口下也全都看得见。
class FwFlowLayout : public QLayout
{
public:
    explicit FwFlowLayout(QWidget *parent = nullptr, int nMargin = 0,
                          int nHSpacing = 2, int nVSpacing = 2);
    ~FwFlowLayout() override;

    void addItem(QLayoutItem *item) override;
    int count() const override;
    QLayoutItem *itemAt(int index) const override;
    QLayoutItem *takeAt(int index) override;
    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int nWidth) const override;
    void setGeometry(const QRect &rect) override;
    QSize sizeHint() const override;
    QSize minimumSize() const override;

private:
    int doLayout(const QRect &rect, bool bTestOnly) const;

    QList<QLayoutItem *> m_items;
    int m_nHSpace;
    int m_nVSpace;
};

// 图标动作条：把 QAction 变成一排固定尺寸的 QToolButton，配合 FwFlowLayout 换行。
// 动作的 enabled/checked 状态由 QAction 统一驱动（syncActionStates() 无需改动）。
class FwActionBar : public QWidget
{
public:
    explicit FwActionBar(QWidget *parent = nullptr);

    void setIconSize(const QSize &size);
    void addAction(QAction *pAction);
    void addSeparator();
    // 兼容原 QToolBar 调用点；本控件固定为图标模式、不可拖动
    void setToolButtonStyle(Qt::ToolButtonStyle style);
    void setMovable(bool bMovable);
    void setFloatable(bool bFloatable);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent *pEvent) override;

private:
    int preferredHeightForWidth(int nWidth) const;

    FwFlowLayout         *m_pLayout = nullptr;
    QList<QToolButton *>  m_buttons;
    QSize                 m_iconSize = QSize(18, 18);
    bool                  m_bInHeightFix = false;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    // 插件拒绝打开时的用户可见说明（接口 v6）。
    // sWhy 非空 = 插件回报了**真实原因**（没有音频输出设备 / 解码器不支持该编码 /
    // 文件已损坏…），按原因说；为空 = 老插件没提供原因，退回通用提示。
    // 独立成静态函数：文案是这次修复的核心（以前一律显示"插件无法显示该文件"），
    // 需要能被测试直接断言。
    static QString pluginFailureText(const QString &sFileName, const QString &sWhy);

    // 自动路由的结论（内容优先、插件声明兜底）。
    // 打成独立结构：调用方（新标签页 / 当前标签页 / 测试）拿到的都是同一份判断，
    // 不必各自再读一次文件头，也便于把"凭什么这么开"显示给用户。
    struct RouteDecision
    {
        QString pluginPath;                  // 选中的插件路径；空 = 没有插件能处理
        QString basis;                       // 判断依据（"文件头 %PDF-" / "按扩展名 .pdf"）
        QString formatName;                  // 识别到的格式名（可能为空）
        bool    fromContent = false;         // true = 结论来自文件内容
        bool    contentOverride = false;     // true = 内容结论与后缀不同，确实改用了别的插件
        FormatResolver::Detection detection; // 完整识别结果（打不开时用于提示）
    };

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
    void onActionViewThumbnailTriggered();
    void onActionShowFilePathTriggered();
    void onActionShowFileNameTriggered();
    void onActionAddFileTriggered();
    void onActionDelFileTriggered();
    void onActionOutputFileTriggered();
    void onActionSaveFileTriggered();
    void onActionVerifyArchiveTriggered();
    void onActionCompactArchiveTriggered();
    void onFileManagerWarning(const QString &sMsg);
    void on_actionAbout_triggered();
    void onActionPluginSettingsTriggered();
    void onActionCryptoKeySettings();
    void on_actionUnloadFile_triggered();

    void onTabCloseRequested(int index);
    void onTabCurrentChanged(int index);
    // P0-2：用户拖动标签后原子地同步文档数组，避免“标签顺序与 m_vTabDocs 失配”
    void onTabBarTabMoved(int from, int to);
    void onTreeViewCustomContextMenu(const QPoint &pos);
    void onThumbnailViewCustomContextMenu(const QPoint &pos);
    void onActionNewTabTriggered();

protected:
    bool FindFile(const QString sDir, QStringList& arrFiles, bool bSubDir);
    QString choosePlugin(QString sFileExt);
    // 打开一个文件时的完整路由：读文件头 -> 内容优先；内容落不到已安装插件时用后缀层。
    // FILEINFO 里没有文件头，所以由它负责读（归档内条目 / 新增条目都走 entryHead）。
    RouteDecision routeForFile(const FILEINFO &info);
    // 上面的纯函数部分：只吃"文件名 + 文件头 + 后缀层结论"，不碰文件系统。
    // 静态是为了单测能直接构造各种文件头（真实归档里造不出"后缀与内容不符"的样本）。
    static RouteDecision decideRoute(const QString &sFileName, const QByteArray &head,
                                     const QString &sByExtPath);
    // 事件过滤器：接管文件列表的鼠标双击（绕开 QAbstractItemView 对首次双击的丢弃）
    bool eventFilter(QObject *obj, QEvent *event) override;
    void populateFilePathView(bool bShowPath);
    void refreshFilePathView();
    // 保存重写归档后，按路径把各标签页缓存的 FILEINFO 更新为最新偏移
    void refreshTabInfosFromManager();
    // 保存成功后的统一善后：作废缩略图缓存、回填标签页偏移、刷新列表与动作状态。
    // Ctrl+S 与“未保存改动”提示里的保存都必须走这里。
    // bContentChanged=false：本次只重写了归档头部（快速保存），条目内容与偏移都没变，
    // 缩略图缓存可以继续复用，不必重新解码。
    void afterArchiveSaved(bool bContentChanged = true);
    // 替换归档文件前的统一准备：等后台 CRC 线程、停播并释放映射、等缩略图读完。
    // 返回 false 表示用户取消或环境不满足（已给出可读提示）。
    // 只重写头部的快速保存不替换文件，因此不需要走这里。
    bool prepareForArchiveReplacement();
    void populateFilePathList(bool bShowPath);
    void populateFilePathTree();
    void populateFilePathThumbnails(bool bShowPath);
    void applyCurrentSortToModel();
    void updateViewActionState();
    // 当前视图（树/平铺/缩略图）的当前项；删除/导出/属性只看这一处，避免各模式各写一套。
    QModelIndex currentFileListIndex() const;
    QStandardItem *currentFileListItem() const;
    QAbstractItemView *activeFileListView() const;
    // 右键菜单：树形与缩略图共用一套构造逻辑，只是 indexAt/弹出位置不同
    void showFileListContextMenu(QAbstractItemView *pView, const QPoint &pos);
    // 缩略图：先全部放类型角标，再以空闲回调逐张解码替换，避免大归档切视图时卡死
    void cancelThumbnailGeneration();
    void invalidateThumbnailCache();
    void scheduleNextThumbnail();
    void processNextThumbnail(quint64 nEpoch);
    // 后台解码完成（GUI 线程）：降 inFlight、校验代数、QPixmap::fromImage 后设置图标。
    void onThumbnailDecoded(const QString &sPath, const QImage &img, quint64 nEpoch);
    // 保存/替换归档前等待后台缩略图读取结束，避免 Windows 下归档句柄让 rename 失败。
    bool waitForThumbnailReads(int nTimeoutMs);
    bool prepareThumbnailsForArchiveWrite(int nTimeoutMs = 5000);
    // 离开树形前记住展开状态（切平铺、切缩略图共用）
    void captureTreeExpansionState();
    static bool isThumbnailImageName(const QString &sFileName);

    // 统一刷新所有工具栏 / 菜单动作（含保存按钮），消除「列表空了但保存可用」的状态错乱
    void syncActionStates();
    void setOperationsEnabled(bool bEnabled);

    void collectExpandedItems(const QModelIndex &index, const QString &sKeyPrefix, QSet<QString> &setExpanded);
    void restoreExpandedItems(const QModelIndex &index, const QString &sKeyPrefix, const QSet<QString> &setExpanded);
    // 取消 / 保存
    bool confirmDiscardOrSaveIfDirty();
    // 校验并加载归档（加载文件与"打开合并结果"共用；失败时保持当前归档不变）
protected:
    void loadArchiveFile(const QString &sFileName);   // 测试派生类需要直接加载归档
    // 把归档里的图片条目解码成缩略图（不是图片/读不出来时返回空 QPixmap）。
    // 与界面展示分离：测试派生类可以直接验证解码结果，不必等后台逐张生成。
    QPixmap renderEntryThumbnail(const FILEINFO &info) const;
    // 文件列表右键菜单：文件行 = 打开 / 打开新标签页 / 标记… / 以文本查看 / 以十六进制查看 /
    // 属性；目录行 = 属性。
    // 返回的菜单归调用方 delete；返回 nullptr 表示该行没有可用操作。
    // （protected 是为了让测试派生类能检查菜单项，不弹出真实的模态菜单。）
    QMenu *buildFileListContextMenu(const QModelIndex &index);
    // 「标记…」：编辑当前条目的标签（v4）。写回失败时弹可见提示，不静默丢弃输入。
    void editEntryTags(const FILEINFO &info);
    // 「属性」数据收集：把 FILEINFO / 目录子树整理成对话框要展示的字段。
    // 与 show 分离，测试派生类可以直接校验数值而不进入模态对话框。
    DlgFileProperty::Data filePropertyData(const FILEINFO &info) const;
    DlgFileProperty::Data folderPropertyData(const QModelIndex &index) const;
    // 「插件设置」两个只读页签的正文（测试可直接断言内容，不必弹模态框）。
    QString pluginStatusText() const;
    QString supportedFormatsText() const;
private:
    // 文件列表筛选（按路径/文件名，大小写不敏感）
    bool matchesFilter(const FILEINFO &info) const;
    static QString typeLabel(const QString &sFileName);
    void saveUiState();
    void restoreUiState();
    // 文件列表双击的统一处理（目录 -> 展开/收起，文件 -> 打开）
    void handleFileListDoubleClick(const QModelIndex &index);
    // 「属性」对话框的数据收集：文件按 FILEINFO，文件夹按当前列表子树汇总
    void showFileProperty(const FILEINFO &info);
    void showFolderProperty(const QModelIndex &index);
    // 递归统计目录节点下的文件 / 子文件夹数量（属性对话框与测试共用）
    static void countFolderItems(const QStandardItem *pDir, int &nFiles, int &nDirs);

    // 窗口标题：显示归档名，并在有未保存改动时追加 "*"
    void updateWindowTitle();
    // 标签页图标：按后缀生成彩色角标（运行时绘制，不依赖额外图片资源）
    static QIcon tabIconForFile(const QString &sFileName);
    // 把「新建标签页 +」按钮贴到最后一个标签右侧（标签增删/窗口缩放后重新定位）
    void updateNewTabButton();
    // 「文件路径」列宽：跟随视口宽，但不低于最小宽度（窄面板出横向滚动条）
    void adjustPathColumnWidth();
    // 停靠面板标题栏的「浮动 / 关闭」按钮：放大到 22×22 并换成主题线性图标
    void polishDockTitleButtons();

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
    QTimer                  *m_filterDebounce = nullptr;   // P1-9：筛选去抖
    QString                  m_sFilter;
    FileManager             *m_fileManager;

    QStackedWidget          *m_swShowArea;
    QStackedWidget          *m_swFilePathViews = nullptr;   // 文件列表容器：树形/平铺(树控件) vs 缩略图
    QTreeView               *m_tvFilePath;
    QListView               *m_lvFilePath = nullptr;        // 缩略图网格
    FwActionBar             *m_tbFilePath;
    QToolButton             *m_btnNewTab = nullptr;   // 贴在标签栏末尾的「+」
    bool                     m_bNewTabBtnPending = false;
    bool                     m_bAdjustingPathColumn = false;   // 防止列宽回调递归
    QAction                 *m_acViewTree;
    QAction                 *m_acViewFlat;
    QAction                 *m_acViewThumbnail = nullptr;
    QAction                 *m_acShowFilePath;
    QAction                 *m_acShowFileName;
    QAction                 *m_acAddFile;
    QAction                 *m_acDelFile;
    QAction                 *m_acOutputFile;
    QAction                 *m_acSaveFile;
    QAction                 *m_acVerify = nullptr;
    // 整理归档（回收“快速保存”留下的空洞；会重写整个归档）
    QAction                 *m_acCompact = nullptr;

    // 文件列表的三种显示模式（树形 / 平铺 / 缩略图）
    enum FileViewMode
    {
        ViewTree = 0,
        ViewFlat,
        ViewThumbnail,
    };
    FileViewMode             m_eViewMode = ViewTree;
    bool                     m_bShowPath = false;

    // 缩略图缓存：条目路径 -> 已解码的图标。归档内容变化（加载/增删/保存）时整体失效。
    QHash<QString, QIcon>    m_mapThumbnailIcons;
    // 本轮缩略图模式下待生成/可定位的条目（QPersistentModelIndex 会跟着排序移动）
    QStringList              m_listThumbnailPending;
    QHash<QString, QPersistentModelIndex> m_mapThumbnailIndex;
    quint64                  m_nThumbnailEpoch = 0;   // 模式切换/重建时作废过期的空闲回调
    int                      m_nThumbnailGenerated = 0;   // 本轮已提交张数（防模型图标无限囤积）
    int                      m_nThumbnailInFlight = 0;    // 已提交、尚未回到 GUI 的任务数
    QSharedPointer<ThumbnailGate> m_thumbGate = QSharedPointer<ThumbnailGate>::create();
    QThreadPool             *m_thumbPool = nullptr;       // 专职缩略图解码，最多 2 线程

    PluginInterface         *m_pCurrentInterface = nullptr;

    // v4：归档内容区的只读内存映射（交给支持 mapped 通道的插件，播放期间保持有效）
    struct MappedInput
    {
        QFile *pFile = nullptr;
        uchar *pData = nullptr;
        qint64 nLen = 0;
        ~MappedInput()
        {
            if (pFile != nullptr)
            {
                if (pData != nullptr) pFile->unmap(pData);
                delete pFile;
            }
        }
    };
    QHash<PluginInterface*, QSharedPointer<MappedInput>> m_mapMappedInputs;
    QSet<QString> m_setMappedCrcPending;
    QSet<QString> m_setMappedCrcVerified;
    // 后台 CRC 校验线程的等待门：保存前要等它归零（Windows 下未关闭的归档句柄
    // 会让 rename 失败）；用共享对象保存计数，窗口析构后线程仍能安全归还。
    struct MappedCrcGate
    {
        QMutex mutex;
        QWaitCondition idle;
        int inFlight = 0;
        std::atomic<bool> cancelRequested{false};   // 保存/析构要求尽快中止
    };
    QSharedPointer<MappedCrcGate> m_crcGate = QSharedPointer<MappedCrcGate>::create();
    quint64 m_nArchiveEpoch = 0;   // 归档切换代数：过期校验结果不再写回新归档
    void checkMappedCrcAsync(const QString &sCrcKey, const QString &sPath,
                             qint64 nPosition, qint64 nLen, quint32 nExpected);
    bool waitForMappedCrc(int nTimeoutMs);
    void releaseMappedInput(PluginInterface *iface);
    void releaseAllMappedInputs();

    QVector<TabDoc>          m_vTabDocs;
    QSet<QWidget*>           m_setPluginWidgets;
    // P0-3：停止失败（后台仍可能访问宿主映射）的实例。它们绝不能进入
    // delete / unmap / QPluginLoader::unload 路径，必须在后续时机重试停止。
    QSet<PluginInterface*>   m_setStopUnsafe;
    // 已从标签页摘除、但因停止失败而无法销毁的独占实例（延期清理）
    QVector<PluginInterface*> m_vtDeferredInstances;
    // v7：每秒非阻塞复查 unsafe 实例的 isPlaybackStopped()，后台线程退出后
    // 自动解除 unsafe、释放映射、销毁延期实例并恢复保存/重载。
    QTimer                  *m_stopUnsafePoll = nullptr;
    // 已经提示过「插件不可用」且用户选择放弃的插件：避免对同一插件反复弹窗
    QSet<QString>            m_setWarnedPlugins;
    QSet<QString>            m_setBuiltinTextPlugins;   // 用户选择“以文本查看”的插件，允许再次兜底
    QSet<QString>            m_setTempFiles;
    QSet<QString>            m_setExpandedBeforeSort;   // 树形点列头排序前记住的展开目录
    QSet<QString>            m_setExpandedTree;
    bool                     m_bResetTreeExpansion = false; // 归档切换后首次建树不恢复旧展开键
    bool                     m_bScanningFiles = false;      // 合并前的目录扫描进行中（禁止关闭窗口）
    QString                  m_sSelectedPathBeforeSort;  // 点列头排序前选中的文件
    QHash<QString, QString>  m_mapTempCache;    // 条目标识 -> 临时文件，重复打开时复用
    QStringList              m_listTempOrder;   // 创建顺序，用于上限淘汰
    quint64                  m_uTempSerial = 0; // 单调递增，保证临时文件名不重复

    QMetaObject::Connection  m_connProgress;
    QMetaObject::Connection  m_connFinished;
    QMetaObject::Connection  m_connCancel;
    QProgressDialog         *m_pActiveProgress = nullptr;
    QElapsedTimer            m_progressTimer;
    quint64                  m_nRenderGen = 0;   // 延迟渲染代数：切换后取消过期的渲染

    void openFileInNewTab(const FILEINFO& info);
    void openFileInCurrentTab(const FILEINFO& info);
    // 右键「以文本查看 / 以十六进制查看」：绕过自动路由，只影响这一个标签页。
    // 这是原先「插件映射」唯一值得保留的能力 —— 映射会永久改掉某个后缀的所有文件，
    // 而这里只对当前文件生效，且不落盘。
    void openFileWithBuiltinViewer(const FILEINFO& info, bool bHex);
    // 新建标签页并挂上已创建好的插件实例（新开文件 / 强制内置查看器共用）
    void addFileTab(const FILEINFO& info, const QString& sPluginPath,
                    PluginInterface* pInterface, bool bOwned);
    void activateTab(int index);
    void renderFile(const FILEINFO& info, PluginInterface* iface);
    void destroyTabAt(int index);
    void stopCurrentPlaybackAndInvalidate();
    // P0-2：标签下标 -> 文档下标。优先按插件窗口指针定位（拖动标签后仍然正确），
    // 占位/空标签页回退到顺序下标。
    int docIndexForTab(int tabIndex) const;
    // P0-3：只有 stopPlaybackAndWait() 确认后台已退出，才允许 unmap / delete / 卸载 DLL。
    // 返回 false 表示该实例必须保留（映射、实例、loader 都不动），等待重试或退出。
    bool ensureInstanceStopped(PluginInterface *iface);
    // 批量尝试停止：返回是否所有实例都可安全销毁
    bool ensureAllInstancesStopped();
    // 非阻塞复查 unsafe 集合；只调用插件 isPlaybackStopped()，不等待解码线程。
    void pollStopUnsafeInstances();
    void closeAllTabs();
    void newEmptyTab();
    bool resolvePlugin(const FILEINFO &info, const RouteDecision &route,
                       QString &sPluginPath, PluginInterface* &pInterface, bool &bOwned);

    // 插件不可用时的提示与兜底：显示可读原因 + 部署自检结果，允许「重试加载」或
    // 「以文本查看」。返回可用模板实例；nullptr 表示用户放弃（此时 *pbUseBuiltinText
    // 为 true 表示应改用内置文本查看器）。
    PluginInterface *promptPluginUnavailable(const QString &sPluginPath, const QString &sFileName,
                                             bool *pbUseBuiltinText);
    // 「插件设置」：一个对话框收纳原先「插件诊断 / 插件管理 / 重新加载插件 / 支持的格式」
    // 四个入口的信息与操作（它们本来就在重复列同一批插件）。
    // nInitialTab：0=状态与依赖，1=支持的格式，2=启用/禁用。
    void showPluginSettings(int nInitialTab = 0);
    // 释放所有标签页后让 PluginManager 重新扫描 plugins/（C5）
    void reloadAllPlugins();

    Ui::MainWindow *ui;
};

#endif // MAINWINDOW_H

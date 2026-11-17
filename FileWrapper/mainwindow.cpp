#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "formatresolver.h"

#include <QLabel>
#include <QApplication>
#include <QFileDialog>
#include "dlgmergefile.h"
#include "dlgsplitfile.h"
#include "dlgpluginmap.h"
#include "builtinviewer.h"
#include "pluginmanager.h"
#include "plugindiag.h"
#include <QDebug>
#include "dlgabout.h"
#include <QVariant>
#include "dlgoutputfile.h"
#include <QMessageBox>
#include <QPushButton>
#include <QAbstractButton>
#include <QDialog>
#include <QDialogButtonBox>
#include <QListWidget>
#include <QListWidgetItem>
#include <QProgressDialog>
#include <QMenu>
#include <QToolButton>
#include <QVBoxLayout>
#include <QSet>
#include <QList>
#include <QMap>
#include <QStyle>
#include <QIcon>
#include <QSignalBlocker>
#include <QDir>
#include <QFileInfo>
#include <QStatusBar>
#include <QDateTime>
#include <QSettings>
#include <QMouseEvent>
#include <utility>          // std::move（C2：把读到的 QByteArray 移动进 PluginFile）
#include <cstring>          // std::memcmp（合并前探测已合并归档）

// 超过该阈值的文件优先走「临时文件 + 路径」通道，避免整块载入内存（P1-2）
static const qint64 kLargeFileThreshold = 128LL * 1024 * 1024;

// 列表项是否为“文件叶子节点”：只有文件节点携带 FILEINFO，目录节点不带数据
static bool isFileLeafItem(const QStandardItem *pItem)
{
    return pItem != nullptr && pItem->data().isValid();
}

// 路径归一化：统一分隔符为 '/'，去掉结尾多余的 '/'（保留盘符根如 "E:/"）
static QString normalizePath(const QString &sPath)
{
    QString s = sPath;
    s.replace('\\', '/');
    while (s.endsWith('/') && s.size() > 1 && s.at(s.size() - 2) != ':')
        s.chop(1);
    return s;
}

// 计算所有条目路径的最长公共“目录”前缀（最后一段是文件名，不参与比较）
// 例如 E:/d/a/x.txt 与 E:/d/b/y.txt -> ["E:", "d"]；跨盘符时返回空
static QStringList commonDirPrefix(const QVector<FILEINFO> &vtInfos)
{
    QStringList common;
    bool bFirst = true;
    for (const FILEINFO &fi : vtInfos)
    {
        QStringList segs = normalizePath(fi.sFilePath).split('/', Qt::SkipEmptyParts);
        int nDirCount = qMax(0, segs.size() - 1);   // 排除文件名段

        if (bFirst)
        {
            for (int i = 0; i < nDirCount; ++i)
                common << segs.at(i);
            bFirst = false;
        }
        else
        {
            int nKeep = 0;
            const int nMax = qMin(common.size(), nDirCount);
            while (nKeep < nMax && common.at(nKeep) == segs.at(nKeep))
                ++nKeep;
            while (common.size() > nKeep)
                common.removeLast();
        }

        if (common.isEmpty())
            break;   // 已无公共前缀（如跨盘符），无需继续比较
    }
    return common;
}

// 单链目录压缩：某目录节点只有唯一一个子目录时，把父子并成一行（a -> b -> c 显示为 "a/b/c"）
static void compactSingleChainDirs(QStandardItem *pDir)
{
    if (pDir == nullptr)
        return;

    // 先自底向上处理已有子树，保证合并进来的子节点也已压缩过
    for (int i = 0; i < pDir->rowCount(); ++i)
    {
        QStandardItem *pChild = pDir->child(i);
        if (pChild != nullptr && !isFileLeafItem(pChild))
            compactSingleChainDirs(pChild);
    }

    while (pDir->rowCount() == 1)
    {
        QStandardItem *pOnly = pDir->child(0);
        if (isFileLeafItem(pOnly))
            break;   // 唯一子节点是文件，链条到底

        pDir->setText(pDir->text() + "/" + pOnly->text());
        pDir->setToolTip(pDir->text());

        // 把孙节点整体上移为该节点的子节点
        while (pOnly->rowCount() > 0)
        {
            QList<QStandardItem*> kids = pOnly->takeRow(0);
            pDir->appendRow(kids);
        }
    }
}

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    // 文件管理器需先于视图初始化：视图初始化时会查询“归档是否已加载”来设置工具栏可用状态
    m_fileManager = new FileManager;

    initWidgetFilePath();
    initWidgetFileContent();

    // 启动时一次性加载所有插件（避免每次双击重复 reload）
    PluginManager::getInstance()->loadAllPlugins();

    // 收集所有插件的 widget 指针，关闭/卸载标签页时据此避免误删插件自身持有的 widget
    for (PluginInterface* iface : PluginManager::getInstance()->getAllInterfaces())
        m_setPluginWidgets.insert(iface->getPluginWidget());

    connect(this, &MainWindow::QMergeFiles, m_fileManager, &FileManager::QMergeFiles);
    connect(this, &MainWindow::QSplitFiles, m_fileManager, &FileManager::QSplitFiles);
    connect(m_fileManager, &FileManager::warningOccurred,
            this, &MainWindow::onFileManagerWarning);

    // 「校验归档」加入文件菜单（不改 .ui，避免影响已有布局）
    QAction *actVerify = new QAction(QStringLiteral("校验归档内容"), ui->menu);
    actVerify->setToolTip(QStringLiteral("逐条目检查内容区 CRC32，检测归档是否被篡改或损坏"));
    connect(actVerify, &QAction::triggered, this, &MainWindow::onActionVerifyArchiveTriggered);
    ui->menu->addAction(actVerify);

    // 「插件诊断」加入插件设置菜单：插件加载失败时用户能自己看到原因与缺哪个依赖
    QAction *actPluginDiag = new QAction(QStringLiteral("插件诊断"), ui->menu_2);
    actPluginDiag->setToolTip(QStringLiteral("查看已加载插件、加载失败原因与缺失的运行依赖"));
    connect(actPluginDiag, &QAction::triggered, this,
            &MainWindow::onActionPluginDiagnosticsTriggered);
    ui->menu_2->addAction(actPluginDiag);

    // 「插件管理」勾选启用/禁用、「重新加载插件」让修改生效（C5）
    QAction *actPluginMgr = new QAction(QStringLiteral("插件管理"), ui->menu_2);
    actPluginMgr->setToolTip(QStringLiteral("勾选启用/禁用插件；禁用的插件不加载，也不接手它的文件后缀"));
    connect(actPluginMgr, &QAction::triggered, this, &MainWindow::onActionPluginManagerTriggered);
    ui->menu_2->addAction(actPluginMgr);

    QAction *actReloadPlugins = new QAction(QStringLiteral("重新加载插件"), ui->menu_2);
    actReloadPlugins->setToolTip(QStringLiteral("重新扫描 plugins/ 目录（会先关闭所有标签页）"));
    connect(actReloadPlugins, &QAction::triggered, this, &MainWindow::onActionReloadPluginsTriggered);
    ui->menu_2->addAction(actReloadPlugins);

    // 「支持的格式」：告诉用户能打开什么、遇到不支持的格式该怎么办（比"插件映射"更直接）
    QAction *actFormats = new QAction(QStringLiteral("支持的格式…"), ui->menu_2);
    actFormats->setToolTip(QStringLiteral("查看已安装插件能打开哪些格式，以及常见不支持格式的处理建议"));
    connect(actFormats, &QAction::triggered, this, &MainWindow::onActionSupportedFormatsTriggered);
    ui->menu_2->addAction(actFormats);

    // 启动时若已有插件加载失败，给一条用户可见的提示（以前只进 qDebug，双击文件毫无反应）
    const int nFailedAtStartup = PluginManager::getInstance()->failures().size();
    if (nFailedAtStartup > 0)
        ui->statusBar->showMessage(
            QStringLiteral("有 %1 个插件加载失败，见「插件设置 → 插件诊断」查看原因")
                .arg(nFailedAtStartup), 20000);

    // 清理上一次会话遗留的临时抽取文件。只删 24 小时以前的陈旧文件，
    // 避免误删同一台机器上另一个正在运行的实例刚抽取出来的大文件。
    QDir tmpDir(QDir::tempPath() + QStringLiteral("/FileWrapper"));
    if (tmpDir.exists())
    {
        const QDateTime cutoff = QDateTime::currentDateTime().addDays(-1);
        const QStringList old = tmpDir.entryList(QStringList() << QStringLiteral("fw_*"), QDir::Files);
        for (const QString &n : old)
        {
            const QFileInfo fi(tmpDir.absoluteFilePath(n));
            if (!fi.lastModified().isValid() || fi.lastModified() < cutoff)
                tmpDir.remove(n);
        }
    }
}

MainWindow::~MainWindow()
{
    // 析构期间不再触发 activateTab，避免销毁标签页时重入渲染逻辑
    disconnect(ui->tabWidget, nullptr, this, nullptr);

    // 先停止后台播放，再销毁标签页，最后清理临时文件
    if (m_pCurrentInterface != nullptr)
    {
        m_pCurrentInterface->stopPlayback();
        m_pCurrentInterface = nullptr;
    }
    for (int i = ui->tabWidget->count() - 1; i >= 0; --i)
        destroyTabAt(i);
    cleanupTempFiles();

    if (m_tvFilePath)
        m_tvFilePath->viewport()->removeEventFilter(this);
    delete m_fileManager;
    delete m_hModelFilePath;
    m_hModelFilePath = nullptr;
    delete ui;
}

void MainWindow::cleanupTempFiles()
{
    discardTempCache();
}

// 切换归档时丢弃本会话抽取出的临时文件：此时后台播放已停止、插件实例已销毁，
// 文件不再被占用。避免旧归档的临时副本一直占着磁盘。
void MainWindow::discardTempCache()
{
    for (const QString &p : m_setTempFiles)
        QFile::remove(p);
    m_setTempFiles.clear();
    m_mapTempCache.clear();
    m_listTempOrder.clear();
}

void MainWindow::onFileManagerWarning(const QString &sMsg)
{
    statusBar()->showMessage(sMsg, 10000);
}

// 初始化【文件列表】窗口
void MainWindow::initWidgetFilePath()
{
    m_hModelFilePath = new QStandardItemModel;
    m_hModelFilePath->setColumnCount(1);
    m_tvFilePath = ui->treeView;
    m_tvFilePath->setModel(m_hModelFilePath);

    // 添加工具栏
    m_tbFilePath = new QToolBar(ui->dwFilePath);
    m_acViewTree = new QAction("树形显示", m_tbFilePath);
    m_acViewFlat = new QAction("平铺显示", m_tbFilePath);
    m_acShowFilePath = new QAction("显示路径", m_tbFilePath);
    m_acShowFileName = new QAction("显示名称", m_tbFilePath);
    m_acAddFile = new QAction("添加文件", m_tbFilePath);
    m_acDelFile = new QAction("删除文件", m_tbFilePath);
    m_acOutputFile = new QAction("导出文件", m_tbFilePath);
    m_acSaveFile = new QAction("保存文件", m_tbFilePath);
    m_acSaveFile->setEnabled(false);

    m_acViewTree->setToolTip("按合并前的目录层级以树形展示");
    m_acViewFlat->setToolTip("不显示层级，每个文件一行");
    m_acShowFilePath->setToolTip("平铺模式下显示文件的完整路径");
    m_acShowFileName->setToolTip("平铺模式下仅显示文件名");

    m_tbFilePath->addAction(m_acViewTree);
    m_tbFilePath->addAction(m_acViewFlat);
    m_tbFilePath->addAction(m_acShowFilePath);
    m_tbFilePath->addAction(m_acShowFileName);
    m_tbFilePath->addAction(m_acAddFile);
    m_tbFilePath->addAction(m_acDelFile);
    m_tbFilePath->addAction(m_acOutputFile);
    m_tbFilePath->addAction(m_acSaveFile);

    connect(m_acViewTree, &QAction::triggered, this, &MainWindow::onActionViewTreeTriggered);
    connect(m_acViewFlat, &QAction::triggered, this, &MainWindow::onActionViewFlatTriggered);
    connect(m_acShowFilePath, &QAction::triggered, this, &MainWindow::onActionShowFilePathTriggered);
    connect(m_acShowFileName, &QAction::triggered, this, &MainWindow::onActionShowFileNameTriggered);
    connect(m_acAddFile, &QAction::triggered, this, &MainWindow::onActionAddFileTriggered);
    connect(m_acDelFile, &QAction::triggered, this, &MainWindow::onActionDelFileTriggered);
    connect(m_acOutputFile, &QAction::triggered, this, &MainWindow::onActionOutputFileTriggered);
    connect(m_acSaveFile, &QAction::triggered, this, &MainWindow::onActionSaveFileTriggered);

    // 将工具栏放置在文件列表栏的上头
    QVBoxLayout *vbLytFilePath = new QVBoxLayout();
    vbLytFilePath->addWidget(m_tbFilePath);
    vbLytFilePath->addWidget(m_tvFilePath);
    ui->dockWidgetContents->setLayout(vbLytFilePath);

    // 默认树形模式，故平铺模式专属的两个按钮置灰；同时统一刷新保存按钮等状态
    syncActionStates();

    // 文件列表右键菜单（含“打开新标签页”）
    m_tvFilePath->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tvFilePath, &QTreeView::customContextMenuRequested,
            this, &MainWindow::onTreeViewCustomContextMenu);

    // 关闭 QTreeView 自带的「双击展开/折叠」，改由 eventFilter 统一处理：
    // 自带行为会在业务侧 setExpanded() 之后再次翻转节点，两者互相抵消。
    m_tvFilePath->setExpandsOnDoubleClick(false);
    m_tvFilePath->viewport()->installEventFilter(this);
}

// 初始化【文件内容】窗口（多标签页）
void MainWindow::initWidgetFileContent()
{
    // 清空 UI 默认自带的占位标签页
    ui->tabWidget->clear();
    ui->tabWidget->setTabsClosable(true);   // 允许关闭标签页
    ui->tabWidget->setMovable(false);

    connect(ui->tabWidget, &QTabWidget::tabCloseRequested,
            this, &MainWindow::onTabCloseRequested);
    connect(ui->tabWidget, &QTabWidget::currentChanged,
            this, &MainWindow::onTabCurrentChanged);

    // 标签栏右上角“+”按钮：新建空白标签页
    QToolButton *btnNewTab = new QToolButton(this);
    btnNewTab->setText("+");
    btnNewTab->setToolTip("新建标签页");
    btnNewTab->setAutoRaise(true);
    connect(btnNewTab, &QToolButton::clicked, this, &MainWindow::onActionNewTabTriggered);
    ui->tabWidget->setCornerWidget(btnNewTab, Qt::TopRightCorner);

    // 默认新建一个空白标签页
    newEmptyTab();
}

// 新建一个空白标签页（无文件）
void MainWindow::newEmptyTab()
{
    TabDoc doc;
    doc.hasFile = false;
    m_vTabDocs.append(doc);

    QWidget *placeholder = new QWidget();
    QVBoxLayout *ly = new QVBoxLayout(placeholder);
    QLabel *lbl = new QLabel(placeholder);
    lbl->setText("空白标签页\n\n在左侧文件列表中双击文件，或右键选择“打开新标签页”以在此查看文件内容。");
    lbl->setAlignment(Qt::AlignCenter);
    lbl->setWordWrap(true);
    ly->addWidget(lbl);

    int newIdx = ui->tabWidget->addTab(placeholder, "新标签页");
    ui->tabWidget->setCurrentIndex(newIdx); // 触发 currentChanged -> activateTab
}

// 关闭全部标签页（用于加载/卸载归档时重置）
void MainWindow::closeAllTabs()
{
    // 断开信号，避免逐个 removeTab 触发多余的 activateTab
    disconnect(ui->tabWidget, &QTabWidget::currentChanged, this, &MainWindow::onTabCurrentChanged);
    disconnect(ui->tabWidget, &QTabWidget::tabCloseRequested, this, &MainWindow::onTabCloseRequested);

    // 先停止当前实例的后台播放，再逐个销毁标签页（避免音视频在实例销毁前仍在后台运行）
    if (m_pCurrentInterface != nullptr)
    {
        m_pCurrentInterface->stopPlayback();
        m_pCurrentInterface = nullptr;
    }

    // 逐个销毁标签页（含各自独占的插件实例，会连带销毁其内部窗口）
    for (int i = ui->tabWidget->count() - 1; i >= 0; --i)
        destroyTabAt(i);

    // 旧归档的临时副本已无人使用，立即回收磁盘（避免反复加载大归档堆积 GB 级副本）
    discardTempCache();

    m_pCurrentInterface = nullptr;

    connect(ui->tabWidget, &QTabWidget::currentChanged, this, &MainWindow::onTabCurrentChanged);
    connect(ui->tabWidget, &QTabWidget::tabCloseRequested, this, &MainWindow::onTabCloseRequested);

    // 重新创建一个空白标签页
    newEmptyTab();
}

// 根据文件后缀选择插件，并为标签页准备插件实例：
// 三级路由：用户自定义映射 -> 插件自描述的 extensions -> 内置兜底；
// 实例一律由 createInstance() 独占创建（B2：不再回退到共享实例）。
bool MainWindow::resolvePlugin(const FILEINFO &info, QString &sPluginPath,
                               PluginInterface* &pInterface, bool &bOwned)
{
    sPluginPath.clear();
    pInterface = nullptr;
    bOwned = false;

    const QString sExt = QFileInfo(info.sFilePath).suffix();
    sPluginPath = choosePlugin(sExt);
    if (sPluginPath.isEmpty())
    {
        // 扩展名不可靠（下载改名、无后缀、.dat/.bin）时，先按**文件内容**识别真实格式：
        // 这是对普通用户最有用的兜底 —— 不需要知道该选哪个插件。
        const QByteArray head = m_fileManager->entryHead(info.sFilePath, 256 * 1024);
        const FormatResolver::Detection det = FormatResolver::detect(info.sFileName, head);

        if (!det.pluginExt.isEmpty())
        {
            const QString sByContent =
                PluginManager::getInstance()->pluginForExtension(det.pluginExt);
            if (!sByContent.isEmpty())
            {
                sPluginPath = sByContent;
                statusBar()->showMessage(
                    QStringLiteral("扩展名无法识别，但文件内容判断为「%1」，已用对应插件打开。")
                        .arg(det.displayName), 12000);
            }
        }

        if (sPluginPath.isEmpty())
        {
            // 仍无法处理：告诉用户"这是什么 + 该怎么办"，而不是只让他去猜该映射哪个插件。
            QMessageBox box(this);
            box.setWindowTitle(QStringLiteral("无法打开该文件"));
            box.setIcon(QMessageBox::Information);

            const QString sType = sExt.isEmpty() ? QStringLiteral("(无扩展名)")
                                                 : QStringLiteral(".") + sExt;
            box.setText(QStringLiteral("「%1」的格式无法直接显示：%2")
                            .arg(info.sFileName, sType));

            QStringList infoLines;
            if (!det.displayName.isEmpty())
                infoLines << QStringLiteral("识别结果：%1").arg(det.displayName);
            if (!det.detail.isEmpty())
                infoLines << QStringLiteral("判断依据：%1").arg(det.detail);
            if (!det.advice.isEmpty())
                infoLines << QStringLiteral("\n建议：%1").arg(det.advice);
            if (!det.pluginExt.isEmpty())
                infoLines << QStringLiteral("\n识别为「%1」，但对应插件当前不可用（可能未部署或被禁用）。"
                                            "可在「插件设置 → 插件诊断 / 插件管理」里检查。")
                                 .arg(det.pluginExt);
            infoLines << QStringLiteral("\n本程序支持：docx/odt/rtf、xlsx/ods、pptx/odp、pdf、"
                                        "epub/fb2/mobi、文本/代码、图片、音视频。");
            box.setInformativeText(infoLines.join(QStringLiteral("\n")));

            QPushButton *bText   = box.addButton(QStringLiteral("以文本查看"), QMessageBox::AcceptRole);
            QPushButton *bHex    = box.addButton(QStringLiteral("以十六进制查看"), QMessageBox::AcceptRole);
            QPushButton *bList   = box.addButton(QStringLiteral("查看支持的格式…"), QMessageBox::ActionRole);
            QPushButton *bMap    = box.addButton(QStringLiteral("设置插件映射…"), QMessageBox::ActionRole);
            QPushButton *bCancel = box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
            box.setDefaultButton(bText);
            box.exec();

            QAbstractButton *clicked = box.clickedButton();
            Q_UNUSED(bCancel);   // 只是用于布局/默认按钮，点击判断不需要它
            if (clicked == bList)
            {
                showSupportedFormats();
                return false;
            }
            if (clicked == bMap)
            {
                DlgPluginMap dlg(this);
                dlg.exec();
                return false;
            }
            if (clicked == bText)      sPluginPath = QStringLiteral("builtin://text");
            else if (clicked == bHex)  sPluginPath = QStringLiteral("builtin://hex");
            else return false;
        }
    }

    if (sPluginPath.startsWith(QStringLiteral("builtin://")))
    {
        const BuiltinViewer::Mode mode = sPluginPath.endsWith(QStringLiteral("hex"))
                                       ? BuiltinViewer::HexMode : BuiltinViewer::TextMode;
        pInterface = new BuiltinViewer(mode);
        bOwned = true;
        return true;
    }

    PluginInterface *pTemplate = PluginManager::getInstance()->getInterface(sPluginPath);
    if (!pTemplate)
    {
        // 插件不可用：给出可读原因 + 部署自检结果，并支持「重试加载 / 以文本查看」。
        // 以前这里只有一行 qDebug，用户双击文件后没有任何反应（2026-11-17 修复）。
        qDebug() << "获取插件接口失败:" << sPluginPath;

        bool bUseBuiltinText = false;
        pTemplate = promptPluginUnavailable(sPluginPath, info.sFileName, &bUseBuiltinText);
        if (bUseBuiltinText)
        {
            // 退回到内置文本查看器（与「未找到插件」对话框里的兜底一致）
            pInterface = new BuiltinViewer(BuiltinViewer::TextMode);
            bOwned = true;
            return true;
        }
        if (pTemplate == nullptr)
            return false;
    }

    QString sCreateErr;
    pInterface = PluginManager::createInstanceFor(pTemplate, &sCreateErr);
    if (pInterface != nullptr)
    {
        bOwned = true;
        return true;
    }

    // B2：以前这里会「降级为共享实例」—— 同类型多标签页会共用同一内容区，
    // 打开 A 文件会把 B 标签页的内容顶掉，而且只留一行 qWarning。现在按打开失败处理，
    // 并给用户可读原因 + 文本查看兜底（与插件加载失败的处理保持一致）。
    qWarning() << "[Plugin] 无法创建插件实例:" << sPluginPath << sCreateErr;

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("插件不可用"));
    box.setIcon(QMessageBox::Warning);
    box.setText(QStringLiteral("插件「%1」无法为「%2」创建查看实例。")
                    .arg(PluginDiagnostics::pluginDisplayName(sPluginPath),
                         info.sFileName.isEmpty() ? QStringLiteral("该文件") : info.sFileName));
    box.setInformativeText(sCreateErr + QStringLiteral(
        "\n\n每个插件都必须为每个标签页创建独立实例；退化成共享实例会让多个标签页互相覆盖内容，"
        "所以这里直接拒绝打开。"));
    QPushButton *bText = box.addButton(QStringLiteral("以文本查看"), QMessageBox::ActionRole);
    QPushButton *bCancel = box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    box.setDefaultButton(bText);
    box.exec();

    if (box.clickedButton() == bText)
    {
        pInterface = new BuiltinViewer(BuiltinViewer::TextMode);
        bOwned = true;
        return true;
    }
    Q_UNUSED(bCancel);
    return false;
}

// 插件不可用时的提示：显示 Qt 的原始错误 + 部署自检（缺哪些依赖、做什么用的），
// 并提供「重试加载 / 以文本查看 / 取消」三种处理。
// 返回：可用模板实例（重试成功后）；nullptr + *pbUseBuiltinText=true 表示改用文本查看；
//       nullptr + *pbUseBuiltinText=false 表示用户放弃。
PluginInterface *MainWindow::promptPluginUnavailable(const QString &sPluginPath,
                                                     const QString &sFileName,
                                                     bool *pbUseBuiltinText)
{
    if (pbUseBuiltinText) *pbUseBuiltinText = false;

    // C5：插件被用户禁用时，给出「去启用」的指引 —— 既不是加载失败，也不是"没插件"
    const QString sPluginId = PluginDiagnostics::pluginId(sPluginPath);
    if (!PluginManager::isPluginEnabled(sPluginId))
    {
        QMessageBox box(this);
        box.setWindowTitle(QStringLiteral("插件已禁用"));
        box.setIcon(QMessageBox::Information);
        box.setText(QStringLiteral("插件「%1」已被禁用，所以「%2」无法用默认方式打开。")
                        .arg(PluginDiagnostics::pluginDisplayName(sPluginPath),
                             sFileName.isEmpty() ? QStringLiteral("该文件") : sFileName));
        box.setInformativeText(QStringLiteral(
            "可以在「插件设置 → 插件管理」里勾选启用，再点「重新加载插件」。"));
        QPushButton *bText = box.addButton(QStringLiteral("以文本查看"), QMessageBox::ActionRole);
        QPushButton *bCancel = box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
        box.setDefaultButton(bCancel);
        box.exec();
        if (box.clickedButton() == bText)
        {
            if (pbUseBuiltinText) *pbUseBuiltinText = true;
            m_setWarnedPlugins.insert(sPluginPath);
            return nullptr;
        }
        m_setWarnedPlugins.insert(sPluginPath);
        return nullptr;
    }

    PluginManager *pMgr = PluginManager::getInstance();

    // 同一插件在本会话已经提示过、且用户当时选择放弃：不再反复弹窗打扰
    if (m_setWarnedPlugins.contains(sPluginPath))
        return nullptr;

    for (;;)   // 用户点「重试加载」后仍失败时重新展示，直到成功或放弃
    {
        // 同时失败的其它插件（帮助判断是不是整批依赖都缺了，例如缺 Qt6Xml 会同时影响两个插件）
        QStringList sOtherFailed;
        for (const PluginManager::Failure &f : pMgr->failures())
            if (f.path != sPluginPath)
                sOtherFailed << PluginDiagnostics::pluginDisplayName(f.path);

        const QString sReport = PluginDiagnostics::buildReport(
                    sPluginPath, pMgr->failureFor(sPluginPath),
                    QCoreApplication::applicationDirPath(), sOtherFailed);

        QMessageBox box(this);
        box.setWindowTitle(QStringLiteral("插件不可用"));
        box.setIcon(QMessageBox::Warning);
        box.setText(QStringLiteral("无法加载插件「%1」，所以「%2」暂时打不开。")
                        .arg(PluginDiagnostics::pluginDisplayName(sPluginPath),
                             sFileName.isEmpty() ? QStringLiteral("该文件") : sFileName));
        box.setInformativeText(sReport);
        QPushButton *bRetry  = box.addButton(QStringLiteral("重试加载"), QMessageBox::AcceptRole);
        QPushButton *bText   = box.addButton(QStringLiteral("以文本查看"), QMessageBox::ActionRole);
        QPushButton *bCancel = box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
        box.setDefaultButton(bRetry);
        box.exec();

        if (box.clickedButton() == bRetry)
        {
            pMgr->retryFailedPlugins();
            PluginInterface *pTemplate = pMgr->getInterface(sPluginPath);
            if (pTemplate != nullptr)
            {
                m_setWarnedPlugins.remove(sPluginPath);   // 恢复正常，允许以后再提示
                return pTemplate;
            }
            continue;                                      // 还是不行：带着新错误再显示一次
        }
        if (box.clickedButton() == bText)
        {
            if (pbUseBuiltinText) *pbUseBuiltinText = true;
            m_setWarnedPlugins.insert(sPluginPath);
            return nullptr;
        }
        Q_UNUSED(bCancel);
        m_setWarnedPlugins.insert(sPluginPath);
        return nullptr;
    }
}

// 「插件设置 → 支持的格式」：一屏告诉用户"本程序能打开什么、常见不支持格式该怎么办"
// 也解释了插件映射什么时候才用得上（只在明确知道该选哪个插件时）。
void MainWindow::showSupportedFormats()
{
    PluginManager *pMgr = PluginManager::getInstance();

    QStringList lines;
    lines << QStringLiteral("已安装插件及可打开的后缀：");
    for (const QString &sId : pMgr->knownPluginIds())
    {
        const QString sPath = pMgr->pluginPathForId(sId);
        if (sPath.isEmpty()) continue;
        QStringList exts = pMgr->extensionsOf(sPath);
        exts.sort();
        QStringList dotted;
        for (const QString &e : exts) dotted << (QStringLiteral(".") + e);
        lines << QStringLiteral("  · %1：%2")
                     .arg(PluginDiagnostics::pluginDisplayName(sPath),
                          dotted.isEmpty() ? QStringLiteral("（未声明后缀）")
                                           : dotted.join(QStringLiteral(" ")));
    }

    lines << QString();
    lines << QStringLiteral("后缀不认识的文件会先按**文件内容**自动识别；仍无法显示时，可以：");
    lines << QStringLiteral("  · 以文本查看 / 以十六进制查看（不会改动文件）");
    lines << QStringLiteral("  · 把它转换成受支持的格式（最省事）");
    lines << QStringLiteral("  · 设置插件映射：只有明确知道该用哪个插件时才需要");

    lines << QString();
    lines << QStringLiteral("常见但本程序暂不支持的格式：");
    for (const auto &pair : FormatResolver::knownUnsupported())
        lines << QStringLiteral("  · %1：%2").arg(pair.first, pair.second);

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("支持的格式"));
    box.setIcon(QMessageBox::Information);
    box.setText(QStringLiteral("本程序能直接显示的格式"));
    box.setInformativeText(lines.join(QStringLiteral("\n")));
    box.setStandardButtons(QMessageBox::Ok);
    box.exec();
}

void MainWindow::onActionSupportedFormatsTriggered()
{
    showSupportedFormats();
}

void MainWindow::onActionPluginDiagnosticsTriggered()
{
    showPluginDiagnostics();
}

// ================= C5: 插件管理 / 重新加载 =================

void MainWindow::onActionPluginManagerTriggered()
{
    showPluginManagerDialog();
}

void MainWindow::onActionReloadPluginsTriggered()
{
    reloadAllPlugins();
}

void MainWindow::showPluginManagerDialog()
{
    PluginManager *pMgr = PluginManager::getInstance();
    const QStringList listIds = pMgr->knownPluginIds();
    if (listIds.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("插件管理"),
                                 QStringLiteral("没有发现任何插件（plugins/ 目录为空或不可读）。"));
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("插件管理"));
    dlg.resize(460, 380);
    auto *pLayout = new QVBoxLayout(&dlg);
    auto *pTip = new QLabel(QStringLiteral(
        "勾选 = 启用。被禁用的插件不会加载，也不会接手它声明过的文件后缀；"
        "保存后需要「重新加载插件」才会生效。"), &dlg);
    pTip->setWordWrap(true);
    pLayout->addWidget(pTip);

    auto *pList = new QListWidget(&dlg);
    for (const QString &sId : listIds)
    {
        auto *pItem = new QListWidgetItem(sId, pList);
        pItem->setFlags(pItem->flags() | Qt::ItemIsUserCheckable);
        pItem->setCheckState(PluginManager::isPluginEnabled(sId) ? Qt::Checked : Qt::Unchecked);
        if (!PluginManager::isPluginEnabled(sId))
            pItem->setToolTip(QStringLiteral("当前被禁用"));
    }
    pLayout->addWidget(pList, 1);

    auto *pButtons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    pLayout->addWidget(pButtons);
    connect(pButtons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(pButtons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted)
        return;

    bool bChanged = false;
    for (int i = 0; i < pList->count(); ++i)
    {
        QListWidgetItem *pItem = pList->item(i);
        const bool bWant = (pItem->checkState() == Qt::Checked);
        if (bWant != PluginManager::isPluginEnabled(pItem->text()))
        {
            PluginManager::setPluginEnabled(pItem->text(), bWant);
            bChanged = true;
        }
    }
    if (!bChanged)
        return;

    if (QMessageBox::question(this, QStringLiteral("插件管理"),
                              QStringLiteral("设置已保存。现在重新加载插件让它生效吗？\n"
                                             "（重新加载会关闭所有标签页）"),
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::Yes) == QMessageBox::Yes)
        reloadAllPlugins();
}

void MainWindow::reloadAllPlugins()
{
    // 重新加载会卸载 DLL：必须先释放所有标签页里的插件实例，否则对象虚表悬空。
    // 只在确实有"打开过文件"的标签页时才需要确认（空标签页没有插件实例）。
    bool bAnyFileTab = false;
    for (const TabDoc &doc : m_vTabDocs)
        if (doc.hasFile) { bAnyFileTab = true; break; }

    if (bAnyFileTab)
    {
        if (QMessageBox::question(this, QStringLiteral("重新加载插件"),
                                  QStringLiteral("重新加载需要先关闭所有标签页（插件实例必须先释放）。\n是否继续？"),
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No) != QMessageBox::Yes)
            return;
    }

    if (m_pCurrentInterface != nullptr)
    {
        m_pCurrentInterface->stopPlayback();
        m_pCurrentInterface = nullptr;
    }
    closeAllTabs();                        // 销毁标签页持有的插件实例
    m_setWarnedPlugins.clear();
    m_setPluginWidgets.clear();

    PluginManager *pMgr = PluginManager::getInstance();
    pMgr->reloadPlugins();

    // 重新收集插件 widget 指针（清理标签页时据此避免误删）
    for (PluginInterface *pIface : pMgr->getAllInterfaces())
        m_setPluginWidgets.insert(pIface->getPluginWidget());

    QString sMsg = QStringLiteral("插件已重新加载：%1 个可用").arg(pMgr->getAllInterfaces().size());
    if (!pMgr->disabledPluginIds().isEmpty())
        sMsg += QStringLiteral("，%1 个被禁用").arg(pMgr->disabledPluginIds().size());
    const int nFailed = pMgr->failures().size();
    if (nFailed > 0)
        sMsg += QStringLiteral("，%1 个加载失败（见「插件诊断」）").arg(nFailed);
    statusBar()->showMessage(sMsg, 15000);
}

// 「插件设置 → 插件诊断」：一屏看清「已加载什么、谁没加载、缺哪些依赖」
void MainWindow::showPluginDiagnostics()
{
    PluginManager *pMgr = PluginManager::getInstance();
    const QString sAppDir = QCoreApplication::applicationDirPath();
    const QList<PluginManager::Failure> listFailures = pMgr->failures();
    const QList<PluginInterface *> listLoaded = pMgr->getAllInterfaces();

    QStringList lines;
    lines << QStringLiteral("程序目录：%1").arg(sAppDir);
    lines << QStringLiteral("运行环境：%1")
                 .arg(PluginDiagnostics::summaryLine(sAppDir, listFailures.size()));
    lines << QStringLiteral("已加载插件：%1 个").arg(listLoaded.size());

    // 加载失败的插件 + 原因
    if (!listFailures.isEmpty())
    {
        lines << QString();
        lines << QStringLiteral("加载失败的插件：");
        for (const PluginManager::Failure &f : listFailures)
        {
            QString sErr = f.error.trimmed();
            if (sErr.isEmpty()) sErr = QStringLiteral("（无错误信息）");
            lines << QStringLiteral("  · %1 —— %2")
                         .arg(PluginDiagnostics::pluginDisplayName(f.path), sErr);
        }
    }

    // 被禁用的插件（C5）：它们不加载，但要让用户知道"为什么这个格式打不开"
    const QStringList listDisabled = pMgr->disabledPluginIds();
    if (!listDisabled.isEmpty())
    {
        lines << QString();
        lines << QStringLiteral("已禁用（不会加载）：%1").arg(listDisabled.join(QStringLiteral("、")));
    }

    // 部署自检：缺哪些依赖、各自做什么用
    lines << QString();
    QString sCheckError;
    const QList<PluginDiagnostics::MissingFile> listMissing =
            PluginDiagnostics::missingFiles(sAppDir, &sCheckError);
    if (!sCheckError.isEmpty())
    {
        lines << QStringLiteral("部署自检：%1").arg(sCheckError);
    }
    else if (listMissing.isEmpty())
    {
        lines << QStringLiteral("部署自检：依赖齐全（清单：%1）")
                     .arg(PluginDiagnostics::manifestName());
    }
    else
    {
        lines << QStringLiteral("部署自检：缺少 %1 个依赖文件（清单：%2）")
                     .arg(listMissing.size()).arg(PluginDiagnostics::manifestName());
        for (const PluginDiagnostics::MissingFile &m : listMissing)
            lines << (m.hint.isEmpty()
                      ? QStringLiteral("  · %1").arg(m.path)
                      : QStringLiteral("  · %1 —— %2").arg(m.path, m.hint));
    }

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("插件诊断"));
    box.setIcon(listFailures.isEmpty() && listMissing.isEmpty()
                ? QMessageBox::Information : QMessageBox::Warning);
    box.setText(QStringLiteral("插件运行环境诊断"));
    box.setInformativeText(lines.join(QLatin1Char('\n')));
    box.exec();
}

// 新建标签页并打开选中的记录项文件（该页独占一个插件实例）
void MainWindow::openFileInNewTab(const FILEINFO &info)
{
    QString sPluginPath;
    PluginInterface *pInterface = nullptr;
    bool bOwned = false;
    if (!resolvePlugin(info, sPluginPath, pInterface, bOwned))
        return;

    TabDoc doc;
    doc.hasFile = true;
    doc.info = info;
    doc.pluginPath = sPluginPath;
    doc.interface = pInterface;
    doc.ownsInterface = bOwned;
    doc.needsRender = true;
    m_vTabDocs.append(doc);

    int newIdx = ui->tabWidget->addTab(new QWidget(), info.sFileName);
    ui->tabWidget->setCurrentIndex(newIdx); // 触发 currentChanged -> activateTab
}

// 在当前激活的标签页打开文件（无激活标签页时退化为新建）
void MainWindow::openFileInCurrentTab(const FILEINFO &info)
{
    int cur = ui->tabWidget->currentIndex();
    if (cur < 0 || cur >= m_vTabDocs.size())
    {
        openFileInNewTab(info);
        return;
    }

    TabDoc doc = m_vTabDocs.at(cur);   // 用副本，避免下方摘除/插入元素后引用失效

    QString sPluginPath;
    PluginInterface *pInterface = nullptr;
    bool bOwned = false;
    if (!resolvePlugin(info, sPluginPath, pInterface, bOwned))
        return;

    // 该页已持有同一插件的独占实例：直接复用，仅替换文件并重新渲染
    if (doc.hasFile && doc.ownsInterface && bOwned
        && doc.interface != nullptr && doc.pluginPath == sPluginPath)
    {
        delete pInterface;             // 刚新建的实例不再需要
        m_vTabDocs[cur].info = info;
        m_vTabDocs[cur].needsRender = true;
        ui->tabWidget->setTabText(cur, info.sFileName);   // 标题跟随新文件
        activateTab(cur);
        return;
    }

    // 其它情况：该页需要换成新的插件实例（可能换了插件类型）。
    // 若旧实例正在播放，先停止，避免其解码线程/音频输出残留。
    if (doc.interface != nullptr && doc.interface == m_pCurrentInterface)
        stopCurrentPlaybackAndInvalidate();

    // 摘页并销毁旧的独占实例（连带其内部窗口）；共享实例与插件窗口不删
    QWidget *pOldPage = ui->tabWidget->widget(cur);
    m_vTabDocs.removeAt(cur);          // 先同步文档列表，使 removeTab 触发的 currentChanged 下标对齐
    ui->tabWidget->removeTab(cur);

    QWidget *pOldOwned = nullptr;
    if (doc.interface != nullptr && doc.ownsInterface)
    {
        pOldOwned = doc.interface->getPluginWidget();
        delete doc.interface;
    }
    if (pOldPage != nullptr && pOldPage != pOldOwned && !m_setPluginWidgets.contains(pOldPage))
        delete pOldPage;

    // 复用原下标位置重建该页，并切回该页
    doc.hasFile = true;
    doc.info = info;
    doc.pluginPath = sPluginPath;
    doc.interface = pInterface;
    doc.ownsInterface = bOwned;
    doc.needsRender = true;
    m_vTabDocs.insert(cur, doc);

    ui->tabWidget->insertTab(cur, new QWidget(), info.sFileName);
    ui->tabWidget->setCurrentIndex(cur); // 触发 currentChanged -> activateTab
    activateTab(cur);                    // 当前页未变化时 currentChanged 不触发，这里兜底显式激活
}

// 销毁某个标签页：先把页面从 tabWidget 摘下，再释放该页独占的插件实例（连带销毁其内部窗口），
// 最后删除自建的占位页面。共享实例与插件（模板实例）窗口一律不删。
void MainWindow::destroyTabAt(int index)
{
    if (index < 0 || index >= m_vTabDocs.size() || index >= ui->tabWidget->count())
        return;

    TabDoc doc = m_vTabDocs.at(index);
    QWidget *pPage = ui->tabWidget->widget(index);

    // 先同步文档列表，再摘页：removeTab 可能触发 currentChanged -> activateTab，
    // 此时 m_vTabDocs 的下标必须已与 tabWidget 对齐。
    m_vTabDocs.removeAt(index);
    ui->tabWidget->removeTab(index);   // removeTab 不删除页面 widget

    // 独占实例：delete 会连带 delete 其内部窗口（即该页面）
    QWidget *pOwnedWidget = nullptr;
    if (doc.interface != nullptr && doc.ownsInterface)
    {
        pOwnedWidget = doc.interface->getPluginWidget();
        delete doc.interface;
    }

    // 其余需要删除的页面：自建占位页（不是插件窗口、也不是已随实例销毁的窗口）
    if (pPage != nullptr && pPage != pOwnedWidget && !m_setPluginWidgets.contains(pPage))
        delete pPage;
}

// 停止当前激活实例的后台播放；对带后台播放的插件，让其所在标签页在下次激活时重新渲染
void MainWindow::stopCurrentPlaybackAndInvalidate()
{
    PluginInterface *pOld = m_pCurrentInterface;
    m_pCurrentInterface = nullptr;
    if (pOld == nullptr)
        return;

    pOld->stopPlayback();

    if (!pOld->hasBackgroundPlayback())
        return;

    // 解码资源已被释放，切回该页需要重新渲染，否则只剩最后一帧/黑屏
    for (int i = 0; i < m_vTabDocs.size(); ++i)
    {
        if (m_vTabDocs[i].interface == pOld)
        {
            m_vTabDocs[i].needsRender = true;
            break;
        }
    }
}

// 大文件抽取到临时文件（流式，内存占用固定 1 MB）
bool MainWindow::extractToTempFile(const FILEINFO &info, QString &sOutPath, QString &sErr,
                                   bool *pbNoSpace)
{
    if (pbNoSpace) *pbNoSpace = false;
    // 同一条目重复打开时复用已抽取的副本，避免为 10 GB 条目反复复制几十 GB
    const QString key = QStringLiteral("%1|%2|%3")
            .arg(info.sFilePath).arg(info.nFileLen).arg(info.nContentPositon);
    auto cached = m_mapTempCache.constFind(key);
    if (cached != m_mapTempCache.constEnd()
        && QFileInfo::exists(cached.value())
        && QFileInfo(cached.value()).size() == info.nFileLen)
    {
        sOutPath = cached.value();
        return true;
    }

    // B4：写临时副本前先看磁盘余量（预留 64 MiB 给其他临时文件与碎片）。
    // 空间不够就明确失败 —— 绝不退化成"整条读进内存"，几 GB 的条目会直接打爆进程。
    constexpr qint64 kSpaceMarginBytes = 64LL * 1024 * 1024;
    const qint64 nNeedBytes = info.nFileLen + kSpaceMarginBytes;
    qint64 nAvailableBytes = -1;
    if (!FileManager::hasFreeSpaceFor(QDir::tempPath(), nNeedBytes, &nAvailableBytes))
    {
        if (pbNoSpace) *pbNoSpace = true;
        sErr = QStringLiteral("临时目录所在磁盘空间不足：需要约 %1（含 64 MiB 预留），"
                              "当前可用 %2。\n临时目录：%3")
                   .arg(FwArc::formatSize(nNeedBytes),
                        FwArc::formatSize(qMax<qint64>(0, nAvailableBytes)),
                        QDir::tempPath());
        return false;
    }

    QDir dir(QDir::tempPath() + QStringLiteral("/FileWrapper"));
    if (!dir.exists() && !dir.mkpath(dir.absolutePath()))
    {
        sErr = QStringLiteral("无法创建临时目录: %1").arg(dir.absolutePath());
        return false;
    }
    // 上限淘汰：最多保留 8 个临时副本。删除失败说明仍被插件占用，
    // 保留在 m_setTempFiles 里等析构时再清，但不再计入缓存与顺序表。
    static const int kMaxTempFiles = 8;
    while (m_listTempOrder.size() >= kMaxTempFiles)
    {
        const QString oldest = m_listTempOrder.takeFirst();
        if (QFile::remove(oldest) || !QFileInfo::exists(oldest))
        {
            m_setTempFiles.remove(oldest);
            for (auto it = m_mapTempCache.begin(); it != m_mapTempCache.end(); )
            {
                if (it.value() == oldest) it = m_mapTempCache.erase(it);
                else ++it;
            }
        }
    }

    const QString name = QStringLiteral("fw_%1_%2_%3")
            .arg(QDateTime::currentMSecsSinceEpoch())
            .arg(++m_uTempSerial)
            .arg(info.sFileName);
    // 文件名里可能含路径分隔符（伪造的归档路径），只保留 basename
    const QString safe = QFileInfo(name).fileName();
    sOutPath = dir.absoluteFilePath(safe);

    if (!m_fileManager->extractEntryToTempFile(info.sFilePath, sOutPath, sErr))
        return false;
    m_setTempFiles.insert(sOutPath);
    m_mapTempCache.insert(key, sOutPath);
    m_listTempOrder.append(sOutPath);
    return true;
}

// 读取文件内容并送入插件渲染
void MainWindow::renderFile(const FILEINFO &info, PluginInterface *iface)
{
    if (iface == nullptr) return;
    iface->setSourceName(info.sFileName);

    // —— P1-2：大文件优先走「临时文件 + 路径」通道，插件可自行流式打开 ——
    if (info.nFileLen >= kLargeFileThreshold && iface->prefersFileByPath())
    {
        QString sTemp, sErr;
        bool bNoSpace = false;
        if (extractToTempFile(info, sTemp, sErr, &bNoSpace))
        {
            if (iface->sendFile(PluginFile::fromPath(sTemp, info.sFileName)))
                return;
            // 插件拒绝该文件路径，回退到内存通道
        }
        else if (bNoSpace)
        {
            // B4：磁盘放不下时**不能**退回内存通道（几 GB 的条目会直接把进程打爆），
            // 直接给用户一个能照做的提示，并清掉内容区。
            QMessageBox::warning(this, QStringLiteral("打不开文件"), sErr);
            iface->sendFileData(nullptr, 0);
            return;
        }
        else
        {
            onFileManagerWarning(QStringLiteral("大文件准备失败，将尝试内存方式：%1").arg(sErr));
        }
    }

    // C2：内存通道直接读进 QByteArray（不再经过 new char[] + delete[] 的中转），
    // 通过 PluginFile 交给插件；迁移到 sendFile() 的插件可以零拷贝取用。
    QString sReadErr;
    QByteArray data = m_fileManager->entryContent(info.sFilePath, &sReadErr);
    if (data.isEmpty())
    {
        // 区分「0 字节空文件」与「读取失败」，并给出可读提示（P3-2 / P1-2）
        // 区分「0 字节空文件」与「读取失败」：entryContent 失败时会给 sReadErr，
        // 为空就说明确实是 0 字节（不要再去看 lastError()，那是别的操作留下的陈旧信息）。
        QString sErr = sReadErr;
        if (sErr.isEmpty())
            sErr = QStringLiteral("文件为空（0 字节），没有可显示的内容");
        onFileManagerWarning(QStringLiteral("打开 %1 失败：%2").arg(info.sFileName, sErr));
        iface->sendFileData(nullptr, 0);
        return;
    }
    // 插件在 sendFileData 返回前会同步复制/使用数据，返回后可安全释放
    iface->sendFile(PluginFile::fromMemory(std::move(data), info.sFileName));
}

// 激活某个标签页：挂载该页独占的插件窗口，并按需渲染其文件
void MainWindow::activateTab(int index)
{
    if (index < 0 || index >= ui->tabWidget->count() || index >= m_vTabDocs.size())
    {
        // 索引无效（如最后一个标签页被移除时的 currentChanged(-1)）：
        // 仍要停止上一个实例的后台播放，避免无人接管的解码线程继续运行
        stopCurrentPlaybackAndInvalidate();
        return;
    }

    TabDoc &doc = m_vTabDocs[index];

    // 空白标签页：停止上一个实例的后台播放，页面保持空白
    if (!doc.hasFile || doc.interface == nullptr)
    {
        stopCurrentPlaybackAndInvalidate();
        return;
    }

    QWidget *w = doc.interface->getPluginWidget();

    // 挂载：保证本页展示的正是本标签页独占的窗口。
    // 摘页/插页会改变 currentIndex 并可能递归触发 currentChanged，
    // 这里先屏蔽信号完成挂载并锁定当前页，避免重入。
    if (ui->tabWidget->widget(index) != w)
    {
        QWidget *pOldPage = ui->tabWidget->widget(index);
        {
            QSignalBlocker blocker(ui->tabWidget);
            ui->tabWidget->removeTab(index);
            ui->tabWidget->insertTab(index, w, doc.info.sFileName);
            ui->tabWidget->setCurrentIndex(index);
        }
        // 仅删除自建占位页；插件窗口（本页独占或插件共享）不删
        if (pOldPage != nullptr && pOldPage != w && !m_setPluginWidgets.contains(pOldPage))
            delete pOldPage;
    }

    // 切换实例时停止上一个实例的后台播放（同类型的两个标签页也是各自独立的实例）
    if (m_pCurrentInterface && m_pCurrentInterface != doc.interface)
        stopCurrentPlaybackAndInvalidate();
    m_pCurrentInterface = doc.interface;

    // 渲染：首次打开、文件被替换，或带后台播放的实例被停播后（缓存已失效）时，
    // 重新读取文件内容并送入插件
    if (doc.needsRender)
    {
        renderFile(doc.info, doc.interface);
        doc.needsRender = false;
    }
}

// 用户点击标签页关闭按钮
void MainWindow::onTabCloseRequested(int index)
{
    if (index < 0 || index >= ui->tabWidget->count() || index >= m_vTabDocs.size())
        return;

    // 关闭正在播放的页前，先停止其后台播放，避免解码线程/音频输出残留
    if (m_vTabDocs.at(index).interface != nullptr
        && m_vTabDocs.at(index).interface == m_pCurrentInterface)
    {
        m_pCurrentInterface->stopPlayback();
        m_pCurrentInterface = nullptr;
    }

    destroyTabAt(index);   // 摘页 + 释放该页独占的插件实例

    if (ui->tabWidget->count() == 0)
        newEmptyTab();     // 始终保持至少一个标签页
}

// 切换标签页
void MainWindow::onTabCurrentChanged(int index)
{
    activateTab(index);
}

// 文件列表右键菜单
void MainWindow::onTreeViewCustomContextMenu(const QPoint &pos)
{
    QModelIndex index = m_tvFilePath->indexAt(pos);
    if (!index.isValid())
        return;
    if (!m_fileManager->isArchiveLoaded())
        return;

    QStandardItem *item = m_hModelFilePath->itemFromIndex(index);
    if (!item)
        return;

    // 目录节点不提供“打开”操作（双击即可展开/折叠）
    if (!isFileLeafItem(item))
        return;

    FILEINFO info = item->data().value<FILEINFO>();

    QMenu menu(this);
    QAction *actOpen = menu.addAction("打开");
    QAction *actOpenNew = menu.addAction("打开新标签页");
    QAction *chosen = menu.exec(m_tvFilePath->viewport()->mapToGlobal(pos));
    if (chosen == actOpen)
        openFileInCurrentTab(info);
    else if (chosen == actOpenNew)
        openFileInNewTab(info);
}

// 标签栏“+”按钮：新建空白标签页
void MainWindow::onActionNewTabTriggered()
{
    newEmptyTab();
}

void MainWindow::on_treeView_doubleClicked(const QModelIndex &index)
{
    handleFileListDoubleClick(index);
}

// 文件列表双击的统一处理：
//   目录节点 -> 展开 / 收起该节点（不打开文件）
//   文件节点 -> 在当前激活的标签页打开该文件
void MainWindow::handleFileListDoubleClick(const QModelIndex &index)
{
    if (!index.isValid())
        return;

    QStandardItem *pItem = m_hModelFilePath->itemFromIndex(index);
    // 目录节点：双击仅做展开 / 折叠，不打开文件
    if (!isFileLeafItem(pItem))
    {
        m_tvFilePath->setExpanded(index, !m_tvFilePath->isExpanded(index));
        return;
    }

    // 双击：在当前激活的标签页打开选中的记录项文件
    FILEINFO fInfo = pItem->data().value<FILEINFO>();
    openFileInCurrentTab(fInfo);
}

// 事件过滤器：在文件列表 viewport 上直接接管「鼠标双击」。
//
// 为什么不能只依赖 doubleClicked 信号：
// 1) QAbstractItemView 只有在按下时该行「已经是 currentIndex」才会把双击
//    派发成 doubleClicked。第一次双击一个尚未选中的行会被静默丢弃 ——
//    表现为「双击文件夹毫无反应」，其实第一次双击只是把它选中了。
// 2) QTreeView 自带的 expandsOnDoubleClick（默认为 true）会在业务侧
//    setExpanded() 之后再次展开/折叠该节点，两者互相抵消 ——
//    表现为「双击文件夹展开后立刻又合上」。
// 在 viewport 层接管双击可同时绕开这两个问题，且对目录与文件一视同仁。
bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (m_tvFilePath && m_hModelFilePath
        && obj == m_tvFilePath->viewport()
        && event->type() == QEvent::MouseButtonDblClick)
    {
        QMouseEvent *me = static_cast<QMouseEvent *>(event);
        const QModelIndex index = m_tvFilePath->indexAt(me->position().toPoint());
        if (index.isValid())
        {
            handleFileListDoubleClick(index);
            return true;   // 已处理，不再走 QTreeView 默认的双击逻辑
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

bool MainWindow::confirmDiscardOrSaveIfDirty()
{
    if (!m_fileManager->isDirty())
        return true;

    QMessageBox::StandardButton btn = QMessageBox::warning(this,
        tr("未保存的改动"),
        tr("当前归档存在未保存的改动，继续操作将丢失这些改动。\n是否先保存？"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    if (btn == QMessageBox::Cancel)
        return false;

    if (btn == QMessageBox::Save)
    {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        bool bOk = m_fileManager->save();
        QApplication::restoreOverrideCursor();
        if (!bOk)
        {
            const QString sReason = m_fileManager->lastError();
            QMessageBox::critical(this, tr("保存失败"),
                tr("保存失败，操作已取消。改动仍保留在内存中，可以重试。\n\n原因：%1")
                   .arg(sReason.isEmpty() ? tr("未知错误") : sReason));
            return false;
        }
    }
    // Discard 或 保存成功：继续
    return true;
}

void MainWindow::on_actionLoadFile_triggered()
{
    if (!confirmDiscardOrSaveIfDirty())
        return;

    QString sFileName = QFileDialog::getOpenFileName(this, tr("加载文件"), "", tr("Class Files (*.dat);;All Files (*.*)"));
    if(sFileName.isEmpty())
        return;

    // —— P1-6：先做完整解析校验，失败时完全不触碰当前归档 / 视图 / 脏状态 ——
    QString sErr;
    if (!m_fileManager->validateMergedFile(sFileName, &sErr))
    {
        QMessageBox::critical(this, QStringLiteral("加载失败"),
            QStringLiteral("不是有效的归档文件或文件已损坏。\n%1\n\n原有归档与未保存的改动保持不变。").arg(sErr));
        return;
    }

    m_fileManager->QLoadMergedFile(sFileName);
    if (!m_fileManager->isArchiveLoaded() || m_fileManager->getFileInfos().isEmpty())
    {
        QMessageBox::critical(this, QStringLiteral("加载失败"),
            QStringLiteral("归档解析失败，原有归档保持不变。\n%1")
               .arg(m_fileManager->lastError()));
        syncActionStates();
        return;
    }

    QVector<FILEINFO> vtFileInfos = m_fileManager->getFileInfos();
    Q_UNUSED(vtFileInfos);

    m_hModelFilePath->setHeaderData(0, Qt::Horizontal, sFileName);
    m_tvFilePath->header()->setVisible(true);

    // 加载成功后重置所有已打开的文件标签页（旧归档的文件路径已失效）
    closeAllTabs();

    populateFilePathView(false);
    syncActionStates();
}

void MainWindow::on_actionMergeFile_triggered()
{
    DlgMergeFile dlgMergeFile(this);
    if(dlgMergeFile.exec() == QDialog::Accepted)
    {
        QStringList qLFilePaths = dlgMergeFile.getFilePaths();
        QString sMergedFilePath = dlgMergeFile.getMergedFilePath();

        QVector<QString> vtFilesPaths;
        foreach(QString sFilePath, qLFilePaths)
        {
            QFileInfo fileinfo(sFilePath);
            if(fileinfo.isDir())
            {
                QStringList arrFiles;
                QStringList ext;
                ext << "*";
                FindFile(sFilePath, ext, arrFiles, true);
                foreach(QString sSubFilePath, arrFiles)
                {
                    vtFilesPaths.push_back(sSubFilePath);
                }
            }
            else if(fileinfo.isFile())
            {
                vtFilesPaths.push_back(sFilePath);
            }
        }

        // —— 进度条：合并可能耗时，显示进度并支持取消 ——
        QProgressDialog dlgProgress(QStringLiteral("正在合并文件..."), QStringLiteral("取消"), 0, 100, this);
        dlgProgress.setWindowTitle(QStringLiteral("合并文件"));
        dlgProgress.setWindowModality(Qt::WindowModal);
        dlgProgress.setMinimumDuration(0);
        dlgProgress.setAutoClose(false);
        dlgProgress.setAutoReset(false);
        dlgProgress.setValue(0);

        // 选中的文件里如果有已合并的 .dat：合并时会把它们拆分成条目，而不是当成普通文件
        int nArchivedInputs = 0;
        for (const QString &sP : vtFilesPaths)
        {
            QFile fProbe(sP);
            if (!fProbe.open(QIODevice::ReadOnly)) continue;
            char magic[4] = { 0, 0, 0, 0 };
            if (fProbe.read(magic, 4) == 4
                && std::memcmp(magic, FwArc::MAGIC, 4) == 0)
                ++nArchivedInputs;
        }
        if (nArchivedInputs > 0)
            dlgProgress.setLabelText(QStringLiteral(
                "正在合并文件（其中 %1 个是已合并归档，将先拆分再合并）...")
                    .arg(nArchivedInputs));

        bool bOk = false;
        QString sResultMsg;
        beginProgressOperation(&dlgProgress, &bOk, &sResultMsg);   // 只替换这三条连接，不影响常驻连接

        dlgProgress.show();
        QApplication::processEvents();
        setOperationsEnabled(false);   // 防止 processEvents 期间重入（P3-2）

        emit QMergeFiles(vtFilesPaths, sMergedFilePath);

        dlgProgress.close();
        endProgressOperation();
        setOperationsEnabled(true);
        syncActionStates();

        if (bOk)
            QMessageBox::information(this, QStringLiteral("合并文件"), sResultMsg);
        else
            QMessageBox::critical(this, QStringLiteral("合并文件"), sResultMsg);
    }
}

bool MainWindow::FindFile(const QString sDir, QStringList& arrFileExts, QStringList& arrFiles, bool bSubDir)
{
    QDir dir(sDir);
    if(!dir.exists())
        return false;

    dir.setFilter(QDir::Dirs | QDir::Files);

    // 文件夹排在前面
    dir.setSorting(QDir::DirsFirst);
    QFileInfoList list = dir.entryInfoList();

    int i = 0;
    QString sFileName;
    QString sSlash = "/";

    do
    {
        QFileInfo fi = list.at(i);
        sFileName = fi.fileName();
        if(!fi.isDir())
        {
            if (CheckFileExt(sFileName, arrFileExts))
            {
                sFileName = QString("%1%2%3").arg(sDir).arg(sSlash).arg(sFileName);
                arrFiles << sFileName;
            }
        }
        else
        {
            if(!(fi.fileName() == "." || fi.fileName() == "..") && bSubDir)
            {
                QString sSubDir = QString("%1%2%3").arg(sDir).arg(sSlash).arg(sFileName);
                if (sSubDir.endsWith(sSlash))
                    sSubDir.chop(1);

                FindFile(fi.filePath(), arrFileExts, arrFiles, bSubDir);
            }
        }

        ++i;

    } while(i < list.size());

    return true;
}

bool MainWindow::CheckFileExt(QString sFileName, QStringList& arrFileExts)
{
    if (arrFileExts.contains("*") == true)
        return true;

    QFileInfo fi(sFileName);
    QString sFileExt = fi.completeSuffix().toUpper();
    if(arrFileExts.indexOf(sFileExt) >= 0)
        return true;

    return false;
}

QString MainWindow::choosePlugin(QString sFileExt)
{
    PluginManager *pMgr = PluginManager::getInstance();

    // 1) 用户自定义「后缀 → 插件」映射优先（P2-2）。
    //    映射表里存的是插件逻辑名（DLL 基名），这里反查真实路径 —— 不再手工拼
    //    "plugins/<name>.dll"：那样在非 Windows 平台是错的，改插件文件名也会静默失效。
    const QString sMapped = DlgPluginMap::mappingFor(sFileExt);
    if (!sMapped.isEmpty())
    {
        if (sMapped.startsWith(QStringLiteral("builtin://"))) return sMapped;
        if (sMapped.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0) return QString();
        return pMgr->pluginPathForId(sMapped);
    }

    // 2) 插件自描述：各插件元数据 JSON 里的 "extensions"（C1）。
    //    这里以前有 6 张表、90 个后缀写死，新增格式必须改主程序并重编；
    //    现在后缀由插件自己声明，加格式只改插件，主程序不用动。
    const QString sDeclared = pMgr->pluginForExtension(sFileExt);
    if (!sDeclared.isEmpty()) return sDeclared;

    // 3) 没有插件认领该后缀：返回空，由调用方弹「未找到插件」对话框
    //    （可临时用内置文本 / 十六进制查看，或去「插件映射」里指定）。
    return QString();
}

void MainWindow::on_actionSplitFile_triggered()
{
    DlgSplitFile dlgSplitFile(this);
    if(dlgSplitFile.exec() == QDialog::Accepted)
    {
        const QString sMergedFilePath = dlgSplitFile.getMergedFilePath();

        // —— P2-3：完整解析 + 逐条合法性校验，与加载使用同一套规则 ——
        FwArc::Meta meta;
        QString sErr;
        if (!FileManager::inspectArchive(sMergedFilePath, meta, sErr))
        {
            QMessageBox::critical(this, QStringLiteral("分割失败"),
                QStringLiteral("不是有效的归档文件或文件已损坏。\n%1").arg(sErr));
            return;
        }
        const bool bIsSaveAsOldPath = dlgSplitFile.getIsSaveAsOldPath();
        QString sSplitFileDir;
        if (!bIsSaveAsOldPath)
            sSplitFileDir = dlgSplitFile.getSplitFileDir();

        // —— P0-2：按原路径写入是危险操作，必须二次确认 ——
        if (bIsSaveAsOldPath)
        {
            int nAbs = 0;
            qint64 nTotal = 0;
            for (const FwArc::Entry &e : meta.entries)
            {
                if (FwArc::isAbsolutePath(e.displayPath)) ++nAbs;
                nTotal += qint64(e.size);
            }
            const QString sWarn = QStringLiteral(
                "将按归档内记录的原始路径把 %1 个文件（共 %2）写回磁盘。\n"
                "其中 %3 个是绝对路径；换机器、换盘符或目录被复用时，可能写到完全无关的位置。\n\n"
                "已存在的目标文件不会被覆盖，程序会自动重命名。是否继续？")
                .arg(meta.entries.size()).arg(FwArc::formatSize(nTotal)).arg(nAbs);
            if (QMessageBox::warning(this, QStringLiteral("按原路径分割"), sWarn,
                                     QMessageBox::Yes | QMessageBox::No,
                                     QMessageBox::No) != QMessageBox::Yes)
                return;
        }

        // —— P1-4：进度 / 取消 / 完成汇总 ——
        QProgressDialog dlgProgress(QStringLiteral("正在分割文件..."), QStringLiteral("取消"), 0, 100, this);
        dlgProgress.setWindowTitle(QStringLiteral("分割文件"));
        dlgProgress.setWindowModality(Qt::WindowModal);
        dlgProgress.setMinimumDuration(0);
        dlgProgress.setAutoClose(false);
        dlgProgress.setAutoReset(false);
        dlgProgress.setValue(0);

        bool bOk = false;
        QString sResultMsg;
        beginProgressOperation(&dlgProgress, &bOk, &sResultMsg);
        dlgProgress.show();
        QApplication::processEvents();
        setOperationsEnabled(false);

        emit QSplitFiles(sMergedFilePath, bIsSaveAsOldPath, sSplitFileDir);

        dlgProgress.close();
        endProgressOperation();
        setOperationsEnabled(true);
        syncActionStates();

        if (sResultMsg.isEmpty()) sResultMsg = QStringLiteral("分割未返回结果（归档可能已损坏）");
        if (bOk) QMessageBox::information(this, QStringLiteral("分割文件"), sResultMsg);
        else     QMessageBox::warning(this, QStringLiteral("分割文件"), sResultMsg);
    }
}

// 按当前模式刷新列表：树形（默认）或平铺
void MainWindow::populateFilePathView(bool bShowPath)
{
    m_bShowPath = bShowPath;

    if (m_bTreeMode)
        populateFilePathTree();
    else
        populateFilePathList(bShowPath);

    updateViewActionState();
}

// 平铺模式：每个文件一行，文本为完整路径或文件名
void MainWindow::populateFilePathList(bool bShowPath)
{
    QVector<FILEINFO> vtFileInfos = m_fileManager->getFileInfos();
    int nRow = 0;
    m_hModelFilePath->removeRows(0, m_hModelFilePath->rowCount());
    for(QVector<FILEINFO>::const_iterator cIt = vtFileInfos.begin(); cIt != vtFileInfos.end(); cIt++, nRow++)
    {
        QStandardItem* hItemName = new QStandardItem(bShowPath ? (*cIt).sFilePath : (*cIt).sFileName);
        hItemName->setToolTip(normalizePath((*cIt).sFilePath));
        QVariant var;
        var.setValue(*cIt);
        hItemName->setData(var);
        m_hModelFilePath->setItem(nRow, 0, hItemName);
    }
}

// 树形模式：把条目里保存的原始路径还原成目录层级
//   根节点 = 所有条目的最长公共父目录（如 E:/study/docs）
//   根下按各条目去掉前缀后的目录段逐级建节点，文件作为叶子
//   只有唯一子目录的链条会被压缩成一行（a -> b -> c 显示为 "a/b/c"）
void MainWindow::populateFilePathTree()
{
    // 先记录重建前已展开的目录，重建后恢复，避免增删一个文件导致整棵树被折叠
    QSet<QString> setExpanded;
    for (int i = 0; i < m_hModelFilePath->rowCount(); ++i)
        collectExpandedItems(m_hModelFilePath->index(i, 0), QString(), setExpanded);

    m_hModelFilePath->removeRows(0, m_hModelFilePath->rowCount());

    QVector<FILEINFO> vtFileInfos = m_fileManager->getFileInfos();
    QStringList prefixSegs = commonDirPrefix(vtFileInfos);
    const QString sRootText = prefixSegs.join('/');

    QIcon iconDir  = style()->standardIcon(QStyle::SP_DirIcon);
    QIcon iconFile = style()->standardIcon(QStyle::SP_FileIcon);

    // 根节点：公共父目录；跨盘符等无公共前缀的情况不建根，直接用盘符/首段作顶层
    QStandardItem *pRoot = nullptr;
    if (!prefixSegs.isEmpty())
    {
        pRoot = new QStandardItem(iconDir, sRootText);
        pRoot->setToolTip(sRootText);
        m_hModelFilePath->setItem(0, 0, pRoot);
    }

    QMap<QString, QStandardItem*> mapDirItems;   // 相对目录路径 -> 目录节点
    for (const FILEINFO &info : vtFileInfos)
    {
        QStringList segs = normalizePath(info.sFilePath).split('/', Qt::SkipEmptyParts);
        if (segs.size() < prefixSegs.size() + 1)
            continue;   // 防御：路径比公共前缀还短（理论上不会出现）

        QStandardItem *pParent = pRoot;
        QString sRelDir;
        for (int i = prefixSegs.size(); i < segs.size() - 1; ++i)
        {
            sRelDir = sRelDir.isEmpty() ? segs.at(i) : sRelDir + "/" + segs.at(i);

            QStandardItem *pDir = mapDirItems.value(sRelDir, nullptr);
            if (pDir == nullptr)
            {
                pDir = new QStandardItem(iconDir, segs.at(i));
                pDir->setToolTip(sRelDir);
                if (pParent != nullptr)
                    pParent->appendRow(pDir);
                else
                    m_hModelFilePath->setItem(m_hModelFilePath->rowCount(), 0, pDir);
                mapDirItems.insert(sRelDir, pDir);
            }
            pParent = pDir;
        }

        QStandardItem *hFile = new QStandardItem(iconFile, info.sFileName);
        hFile->setToolTip(normalizePath(info.sFilePath));
        QVariant var;
        var.setValue(info);
        hFile->setData(var);

        if (pParent != nullptr)
            pParent->appendRow(hFile);
        else
            m_hModelFilePath->setItem(m_hModelFilePath->rowCount(), 0, hFile);
    }

    // 单链目录压缩
    for (int i = 0; i < m_hModelFilePath->rowCount(); ++i)
        compactSingleChainDirs(m_hModelFilePath->item(i, 0));

    // 展开状态：有历史记录则恢复；首次加载只展开根与顶层
    if (setExpanded.isEmpty())
    {
        m_tvFilePath->expandToDepth(0);
        if (pRoot != nullptr)
            m_tvFilePath->setExpanded(m_hModelFilePath->indexFromItem(pRoot), true);
    }
    else
    {
        for (int i = 0; i < m_hModelFilePath->rowCount(); ++i)
            restoreExpandedItems(m_hModelFilePath->index(i, 0), QString(), setExpanded);
        if (pRoot != nullptr)
            m_tvFilePath->setExpanded(m_hModelFilePath->indexFromItem(pRoot), true);
    }
}

// 按「归档是否已加载 + 当前展示模式」刷新工具栏动作可用状态
void MainWindow::updateViewActionState()
{
    const bool bLoaded = m_fileManager->isArchiveLoaded();

    m_acViewTree->setEnabled(bLoaded && !m_bTreeMode);
    m_acViewFlat->setEnabled(bLoaded && m_bTreeMode);
    // 「显示路径/显示名称」仅在平铺模式下有意义
    m_acShowFilePath->setEnabled(bLoaded && !m_bTreeMode && !m_bShowPath);
    m_acShowFileName->setEnabled(bLoaded && !m_bTreeMode && m_bShowPath);
}

// 唯一的状态刷新入口：包含保存按钮与所有菜单动作（P1-5 / P1-6）
void MainWindow::syncActionStates()
{
    const bool bLoaded = m_fileManager->isArchiveLoaded();
    const bool bBusy   = m_fileManager->isBusy();

    updateViewActionState();
    m_acSaveFile->setEnabled(bLoaded && m_fileManager->isDirty() && !bBusy);
    m_acAddFile->setEnabled(bLoaded && !bBusy);
    m_acDelFile->setEnabled(bLoaded && !bBusy);
    m_acOutputFile->setEnabled(bLoaded && !bBusy);

    ui->actionLoadFile->setEnabled(!bBusy);
    ui->actionUnloadFile->setEnabled(!bBusy);
    ui->actionMergeFile->setEnabled(!bBusy);
    ui->actionSplitFile->setEnabled(!bBusy);
    ui->actionPluginMap->setEnabled(!bBusy);
}

// 长操作期间禁用可重入动作（P3-2）
void MainWindow::setOperationsEnabled(bool bEnabled)
{
    const bool bLoaded = m_fileManager->isArchiveLoaded();
    const bool bBusy   = !bEnabled;
    ui->actionLoadFile->setEnabled(bEnabled);
    ui->actionUnloadFile->setEnabled(bEnabled);
    ui->actionMergeFile->setEnabled(bEnabled);
    ui->actionSplitFile->setEnabled(bEnabled);
    ui->actionPluginMap->setEnabled(bEnabled);
    m_acAddFile->setEnabled(bEnabled && bLoaded);
    m_acDelFile->setEnabled(bEnabled && bLoaded);
    m_acOutputFile->setEnabled(bEnabled && bLoaded);
    m_acSaveFile->setEnabled(bEnabled && bLoaded && m_fileManager->isDirty() && !bBusy);
}

// 统一管理进度对话框的三条连接，避免 disconnect(sender, nullptr, ...) 误伤常驻连接
void MainWindow::beginProgressOperation(QProgressDialog *dlg, bool *pbOk, QString *psResultMsg)
{
    endProgressOperation();
    m_pActiveProgress = dlg;
    *pbOk = false;
    psResultMsg->clear();

    // 进度文字左对齐，让「正在合并:」「正在分割:」这类固定前缀稳定贴在左侧。
    //
    // QProgressDialog 的状态 label 默认是 AlignHCenter。对话框宽度在首次 show 后固定，
    // 但状态文本长度随「正在合并: <文件名> (<已写>/<总量>)」变化，文本越宽、居中后
    // 起始位置越靠左，于是「正在合并:」这几个字会随着文件名长短左右漂移。
    // 改为左对齐后前缀始终停在左侧，且与下方进度条左端对齐。
    if (QLabel *pStatusLabel = dlg->findChild<QLabel*>())
        pStatusLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    m_connProgress = connect(m_fileManager, &FileManager::progressChanged, this,
        [this](qint64 cur, qint64 total, const QString &status) {
            if (m_pActiveProgress == nullptr) return;
            if (total > 0) m_pActiveProgress->setValue(int(100 * cur / total));
            m_pActiveProgress->setLabelText(status);
        });
    m_connFinished = connect(m_fileManager, &FileManager::operationFinished, this,
        [this, pbOk, psResultMsg](bool ok, const QString &msg) {
            if (m_pActiveProgress) { m_pActiveProgress->setValue(100); m_pActiveProgress->close(); }
            *pbOk = ok;
            *psResultMsg = msg;
        });
    m_connCancel = connect(dlg, &QProgressDialog::canceled,
                           m_fileManager, &FileManager::cancelOperation);
}

void MainWindow::endProgressOperation()
{
    disconnect(m_connProgress);
    disconnect(m_connFinished);
    disconnect(m_connCancel);
    m_connProgress = QMetaObject::Connection();
    m_connFinished = QMetaObject::Connection();
    m_connCancel   = QMetaObject::Connection();
    m_pActiveProgress = nullptr;
}

void MainWindow::onActionVerifyArchiveTriggered()
{
    if (!m_fileManager->isArchiveLoaded())
    {
        QMessageBox::information(this, QStringLiteral("校验归档"),
                                 QStringLiteral("请先加载一个归档文件。"));
        return;
    }
    QProgressDialog dlg(QStringLiteral("正在校验归档内容..."), QStringLiteral("取消"), 0, 100, this);
    dlg.setWindowTitle(QStringLiteral("校验归档"));
    dlg.setWindowModality(Qt::WindowModal);
    dlg.setMinimumDuration(0);
    dlg.setAutoClose(false);
    dlg.setAutoReset(false);
    dlg.setValue(0);

    bool bDummy = false;
    QString sDummy;
    beginProgressOperation(&dlg, &bDummy, &sDummy);
    dlg.show();
    QApplication::processEvents();
    setOperationsEnabled(false);

    QString sReport;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool bOk = m_fileManager->verifyArchive(&sReport);
    QApplication::restoreOverrideCursor();

    dlg.close();
    endProgressOperation();
    setOperationsEnabled(true);
    syncActionStates();
    if (bOk) QMessageBox::information(this, QStringLiteral("校验归档"), sReport);
    else     QMessageBox::warning(this, QStringLiteral("校验归档"), sReport);
}

// 递归收集当前已展开的节点（键 = 从根到该节点的文本链）
void MainWindow::collectExpandedItems(const QModelIndex &index, const QString &sKeyPrefix, QSet<QString> &setExpanded)
{
    if (!index.isValid())
        return;

    QString sKey = sKeyPrefix.isEmpty() ? index.data().toString()
                                        : sKeyPrefix + "/" + index.data().toString();

    if (m_hModelFilePath->hasChildren(index) && m_tvFilePath->isExpanded(index))
        setExpanded.insert(sKey);

    const int nChild = m_hModelFilePath->rowCount(index);
    for (int i = 0; i < nChild; ++i)
        collectExpandedItems(m_hModelFilePath->index(i, 0, index), sKey, setExpanded);
}

// 递归恢复展开状态
void MainWindow::restoreExpandedItems(const QModelIndex &index, const QString &sKeyPrefix, const QSet<QString> &setExpanded)
{
    if (!index.isValid())
        return;

    QString sKey = sKeyPrefix.isEmpty() ? index.data().toString()
                                        : sKeyPrefix + "/" + index.data().toString();

    if (setExpanded.contains(sKey) && m_hModelFilePath->hasChildren(index))
        m_tvFilePath->setExpanded(index, true);

    const int nChild = m_hModelFilePath->rowCount(index);
    for (int i = 0; i < nChild; ++i)
        restoreExpandedItems(m_hModelFilePath->index(i, 0, index), sKey, setExpanded);
}

void MainWindow::refreshFilePathView()
{
    populateFilePathView(m_bShowPath);
}

void MainWindow::onActionViewTreeTriggered()
{
    if (m_bTreeMode)
        return;
    m_bTreeMode = true;
    populateFilePathView(m_bShowPath);
}

void MainWindow::onActionViewFlatTriggered()
{
    if (!m_bTreeMode)
        return;
    m_bTreeMode = false;
    populateFilePathView(m_bShowPath);
}

void MainWindow::onActionShowFilePathTriggered()
{
    populateFilePathView(true);
}

void MainWindow::onActionShowFileNameTriggered()
{
    populateFilePathView(false);
}

void MainWindow::onActionAddFileTriggered()
{
    if(!m_fileManager->isArchiveLoaded())
    {
        QMessageBox::information(this, tr("添加文件"),
                                 tr("请先通过“加载文件”打开一个归档文件，再添加文件。"));
        return;
    }

    QStringList qLFilePaths = QFileDialog::getOpenFileNames(this, tr("添加文件"),
                                                           "", tr("All Files (*.*)"));
    if(qLFilePaths.isEmpty())
        return;

    QVector<QString> vtAdd;
    foreach(QString s, qLFilePaths)
    {
        QFileInfo fi(s);
        if(fi.isFile())
            vtAdd.append(s);
    }
    if(vtAdd.isEmpty())
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_fileManager->addFiles(vtAdd);
    QApplication::restoreOverrideCursor();

    refreshFilePathView();
    syncActionStates();
}

void MainWindow::onActionDelFileTriggered()
{
    if(!m_fileManager->isArchiveLoaded())
    {
        QMessageBox::information(this, tr("删除文件"),
                                 tr("请先通过“加载文件”打开一个归档文件。"));
        return;
    }

    // 树形模式下目录节点不可删除，需选中具体文件
    QStandardItem *pItem = m_hModelFilePath->itemFromIndex(m_tvFilePath->currentIndex());
    if (!isFileLeafItem(pItem))
    {
        QMessageBox::information(this, tr("删除文件"),
                                 tr("请先在列表中选择要删除的文件（目录节点不可删除）。"));
        return;
    }
    FILEINFO fileInfo = pItem->data().value<FILEINFO>();

    QApplication::setOverrideCursor(Qt::WaitCursor);
    bool bOk = m_fileManager->deleteFile(fileInfo.sFilePath);
    QApplication::restoreOverrideCursor();

    if(!bOk)
    {
        QMessageBox::critical(this, tr("删除文件"),
                              tr("删除失败，归档文件可能已损坏。"));
        return;
    }

    refreshFilePathView();
    syncActionStates();
}

void MainWindow::onActionOutputFileTriggered()
{
    // 树形模式下目录节点不可导出，需选中具体文件
    QStandardItem *pItem = m_hModelFilePath->itemFromIndex(m_tvFilePath->currentIndex());    if (!isFileLeafItem(pItem))
    {
        QMessageBox::information(this, tr("导出文件"),
                                 tr("请先在列表中选择要导出的文件（目录节点不可导出）。"));
        return;
    }
    FILEINFO fileInfo = pItem->data().value<FILEINFO>();

    DlgOutputFile dlgOutputFile(this);
    if(dlgOutputFile.exec() == QDialog::Accepted)
    {
        const QString sOutputDir = dlgOutputFile.getOutputDir();

        QProgressDialog dlgProgress(QStringLiteral("正在导出文件..."), QStringLiteral("取消"), 0, 100, this);
        dlgProgress.setWindowTitle(QStringLiteral("导出文件"));
        dlgProgress.setWindowModality(Qt::WindowModal);
        dlgProgress.setMinimumDuration(0);
        dlgProgress.setAutoClose(false);
        dlgProgress.setAutoReset(false);
        dlgProgress.setValue(0);
        bool bDummy = false;
        QString sDummy;
        beginProgressOperation(&dlgProgress, &bDummy, &sDummy);
        dlgProgress.show();
        QApplication::processEvents();
        setOperationsEnabled(false);

        m_fileManager->outputFile(fileInfo.sFilePath, sOutputDir);
        const QString sErr = m_fileManager->lastError();

        dlgProgress.close();
        endProgressOperation();
        setOperationsEnabled(true);
        syncActionStates();

        if (!sErr.isEmpty())
            QMessageBox::warning(this, QStringLiteral("导出文件"), sErr);
        else
            QMessageBox::information(this, QStringLiteral("导出文件"),
                                     QStringLiteral("导出完成：%1").arg(fileInfo.sFileName));
    }
}

void MainWindow::onActionSaveFileTriggered()
{
    if(!m_fileManager->isArchiveLoaded())
    {
        QMessageBox::information(this, tr("保存文件"),
                                 tr("没有已加载的归档文件，无法保存。"));
        return;
    }

    // —— 进度条：保存（重写归档）可能耗时，显示进度并支持取消 ——
    QProgressDialog dlgProgress(tr("正在保存文件..."), tr("取消"), 0, 100, this);
    dlgProgress.setWindowTitle(tr("保存文件"));
    dlgProgress.setWindowModality(Qt::WindowModal);
    dlgProgress.setMinimumDuration(0);
    dlgProgress.setAutoClose(false);
    dlgProgress.setAutoReset(false);
    dlgProgress.setValue(0);

    bool bDummy = false;
    QString sDummy;
    beginProgressOperation(&dlgProgress, &bDummy, &sDummy);

    dlgProgress.show();
    QApplication::processEvents();
    setOperationsEnabled(false);

    const bool bOk = m_fileManager->save();
    const QString sReason = m_fileManager->lastError();

    dlgProgress.close();
    endProgressOperation();
    setOperationsEnabled(true);
    syncActionStates();

    if(!bOk)
    {
        QString sMsg = m_fileManager->isCancelRequested()
                ? tr("保存已取消。改动仍保留在内存中，可重试。")
                : tr("保存失败。改动仍保留在内存中，可重试。\n\n原因：%1")
                    .arg(sReason.isEmpty() ? tr("未知错误") : sReason);
        QMessageBox::critical(this, tr("保存文件"), sMsg);
        return;
    }

    // 保存后刷新列表（重写会更新内存中的内容偏移），并按脏标记更新保存按钮
    refreshFilePathView();
    syncActionStates();
}

void MainWindow::on_actionAbout_triggered()
{
    DlgAbout dlgAbout;
    dlgAbout.exec();
}

void MainWindow::on_actionPluginMap_triggered()
{
    DlgPluginMap dlg(this);
    dlg.exec();
}

void MainWindow::on_actionUnloadFile_triggered()
{
    if (!confirmDiscardOrSaveIfDirty())
        return;

    m_fileManager->unLoadMergedFile();
    m_hModelFilePath->removeRows(0, m_hModelFilePath->rowCount());
    m_tvFilePath->header()->setVisible(false);
    syncActionStates();

    // 卸载归档后关闭所有已打开的文件标签页
    closeAllTabs();
}

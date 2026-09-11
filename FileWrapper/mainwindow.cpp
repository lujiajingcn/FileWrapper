#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QLabel>
#include <QApplication>
#include <QFileDialog>
#include "dlgmergefile.h"
#include "dlgsplitfile.h"
#include "pluginmanager.h"
#include <QDebug>
#include "dlgabout.h"
#include <QVariant>
#include "dlgoutputfile.h"
#include <QMessageBox>
#include <QProgressDialog>
#include <QMenu>
#include <QToolButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QSet>
#include <QList>
#include <QMap>
#include <QStyle>
#include <QIcon>

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
        QStringList segs = normalizePath(fi.sFilePath).split('/', QString::SkipEmptyParts);
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
}

MainWindow::~MainWindow()
{
    delete m_fileManager;
    delete m_hModelFilePath;
    delete ui;
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

    // 默认树形模式，故平铺模式专属的两个按钮置灰
    updateViewActionState();

    // 文件列表右键菜单（含“打开新标签页”）
    m_tvFilePath->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tvFilePath, &QTreeView::customContextMenuRequested,
            this, &MainWindow::onTreeViewCustomContextMenu);
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
    // 先把所有“正作为某标签页页面”的插件 widget 卸下（setParent(nullptr)），
    // 否则 ui->tabWidget->clear() 会删除它们，而插件 widget 由插件自身持有生命周期，误删会导致崩溃。
    for (int i = 0; i < ui->tabWidget->count(); ++i)
    {
        QWidget *w = ui->tabWidget->widget(i);
        if (m_setPluginWidgets.contains(w))
            w->setParent(nullptr);
    }

    // 断开信号，避免逐个 removeTab 触发多余的 activateTab
    disconnect(ui->tabWidget, &QTabWidget::currentChanged, this, &MainWindow::onTabCurrentChanged);
    disconnect(ui->tabWidget, &QTabWidget::tabCloseRequested, this, &MainWindow::onTabCloseRequested);

    ui->tabWidget->clear();
    m_vTabDocs.clear();
    m_pMountedWidget = nullptr;
    m_pCurrentInterface = nullptr;

    connect(ui->tabWidget, &QTabWidget::currentChanged, this, &MainWindow::onTabCurrentChanged);
    connect(ui->tabWidget, &QTabWidget::tabCloseRequested, this, &MainWindow::onTabCloseRequested);

    // 重新创建一个空白标签页
    newEmptyTab();
}

// 根据文件后缀选择插件并解析接口；失败返回空指针并通过 outIface 返回接口
// （复用已有的 choosePlugin + PluginManager 逻辑）
bool MainWindow::resolvePlugin(const FILEINFO &info, QString &sPluginPath, PluginInterface* &pInterface)
{
    sPluginPath.clear();
    pInterface = nullptr;
    QString sExt = QFileInfo(info.sFilePath).suffix();
    sPluginPath = choosePlugin(sExt);
    if (sPluginPath.isEmpty())
    {
        QMessageBox::information(this, "", "没有插件来处理该类型文件，请手动映射插件");
        return false;
    }
    pInterface = PluginManager::getInstance()->getInterface(sPluginPath);
    if (!pInterface)
    {
        qDebug() << "获取插件接口失败:" << sPluginPath;
        return false;
    }
    return true;
}

// 新建标签页并打开选中的记录项文件
void MainWindow::openFileInNewTab(const FILEINFO &info)
{
    QString sPluginPath;
    PluginInterface *pInterface = nullptr;
    if (!resolvePlugin(info, sPluginPath, pInterface))
        return;

    TabDoc doc;
    doc.hasFile = true;
    doc.info = info;
    doc.pluginPath = sPluginPath;
    doc.interface = pInterface;
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

    QString sPluginPath;
    PluginInterface *pInterface = nullptr;
    if (!resolvePlugin(info, sPluginPath, pInterface))
        return;

    m_vTabDocs[cur].hasFile = true;
    m_vTabDocs[cur].info = info;
    m_vTabDocs[cur].pluginPath = sPluginPath;
    m_vTabDocs[cur].interface = pInterface;

    // 当前页可能未变化，currentChanged 不会触发，这里显式激活
    activateTab(cur);
}

// 读取文件内容并送入插件渲染
void MainWindow::renderFile(const FILEINFO &info, PluginInterface *iface)
{
    char *szBuf = nullptr;
    qint64 nFileLen = 0;
    m_fileManager->QGetFileContent(info.sFilePath, &szBuf, nFileLen);
    if (szBuf == nullptr)
    {
        qDebug() << "获取文件内容失败:" << info.sFilePath;
        return;
    }
    // 插件在 sendFileData 返回前会同步复制/使用数据，返回后可安全释放
    iface->sendFileData(szBuf, nFileLen);
    delete[] szBuf;
}

// 激活某个标签页：将对应插件 widget 迁移/挂载到该页，并渲染其文件
void MainWindow::activateTab(int index)
{
    if (index < 0 || index >= ui->tabWidget->count() || index >= m_vTabDocs.size())
    {
        m_pMountedWidget = nullptr;
        m_pCurrentInterface = nullptr;
        return;
    }

    TabDoc &doc = m_vTabDocs[index];

    // 空白标签页：停止上一个插件的后台播放，不挂载任何插件 widget
    if (!doc.hasFile)
    {
        if (m_pCurrentInterface)
            m_pCurrentInterface->stopPlayback();
        m_pCurrentInterface = nullptr;
        m_pMountedWidget = nullptr;
        return;
    }

    PluginInterface *iface = doc.interface;
    QWidget *w = iface->getPluginWidget();

    // 1) 若此插件 widget 当前正挂载在别的标签页（同类型多标签页场景），
    //    先把它从那个标签页拆下，换成一个占位 widget。
    for (int i = 0; i < ui->tabWidget->count(); ++i)
    {
        if (i == index)
            continue;
        if (ui->tabWidget->widget(i) == w)
        {
            QWidget *old = ui->tabWidget->widget(i);
            ui->tabWidget->removeTab(i);
            ui->tabWidget->insertTab(i, new QWidget(),
                                    m_vTabDocs[i].hasFile ? m_vTabDocs[i].info.sFileName
                                                         : QStringLiteral("新标签页"));
            // old 即插件 widget，由插件持有生命周期，不删除
            Q_UNUSED(old);
        }
    }

    // 2) 把该插件 widget 挂载到当前激活的标签页
    QWidget *cur = ui->tabWidget->widget(index);
    if (cur != w)
    {
        QWidget *old = ui->tabWidget->widget(index);
        ui->tabWidget->removeTab(index);
        ui->tabWidget->insertTab(index, w, doc.info.sFileName);
        // 仅删除占位 widget；插件 widget 不删（由插件持有）
        if (old != nullptr && !m_setPluginWidgets.contains(old))
            delete old;
    }

    m_pMountedWidget = w;

    // 3) 切换插件类型时，停止上一个插件的后台播放（视频/音频）
    if (m_pCurrentInterface && m_pCurrentInterface != iface)
        m_pCurrentInterface->stopPlayback();
    m_pCurrentInterface = iface;

    // 4) 渲染文件内容
    renderFile(doc.info, iface);
}

// 用户点击标签页关闭按钮
void MainWindow::onTabCloseRequested(int index)
{
    if (index < 0 || index >= ui->tabWidget->count())
        return;

    QWidget *w = ui->tabWidget->widget(index);
    bool bIsPluginWidget = m_setPluginWidgets.contains(w);

    // 先同步文档列表（使后续 currentChanged 引用的索引与 tabWidget 一致）
    m_vTabDocs.removeAt(index);
    ui->tabWidget->removeTab(index);   // 可能触发 currentChanged -> activateTab

    // 占位 widget 删除；插件 widget 不删（由插件持有），留待后续标签页复用
    if (!bIsPluginWidget)
        delete w;

    if (ui->tabWidget->count() == 0)
    {
        m_pMountedWidget = nullptr;
        m_pCurrentInterface = nullptr;
        newEmptyTab(); // 始终保持至少一个标签页
    }
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
            QMessageBox::critical(this, tr("保存失败"),
                                  tr("保存失败，归档文件可能已损坏或磁盘写入失败。操作已取消。"));
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

    m_fileManager->QLoadMergedFile(sFileName);

    QVector<FILEINFO> vtFileInfos = m_fileManager->getFileInfos();
    if(vtFileInfos.isEmpty())
    {
        m_hModelFilePath->removeRows(0, m_hModelFilePath->rowCount());
        m_tvFilePath->header()->setVisible(false);
        updateViewActionState();
        QMessageBox::critical(this, "加载失败", "不是有效的归档文件或文件已损坏。\n"
                                "（缺少 FWDA 标识、版本不兼容或头部校验失败）");
        return;
    }

    m_hModelFilePath->setHeaderData(0, Qt::Horizontal, sFileName);
    m_tvFilePath->header()->setVisible(true);

    // 加载成功后重置所有已打开的文件标签页（旧归档的文件路径已失效）
    closeAllTabs();

    populateFilePathView(false);
    m_acSaveFile->setEnabled(false);
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
        QProgressDialog dlgProgress(tr("正在合并文件..."), tr("取消"), 0, 100, this);
        dlgProgress.setWindowTitle(tr("合并文件"));
        dlgProgress.setWindowModality(Qt::WindowModal);
        dlgProgress.setMinimumDuration(0);  // 立即显示，不等待 4 秒默认延迟
        dlgProgress.setAutoClose(false);
        dlgProgress.setAutoReset(false);
        dlgProgress.setValue(0);

        // 清理上一轮可能残留的进度连接：lambda 捕获的是上一轮已销毁的局部对话框，
        // 若不清理，下一轮再次 emit 进度时会访问悬空引用导致崩溃。
        disconnect(m_fileManager, nullptr, this, nullptr);

        bool bOk = false;
        QString sResultMsg;
        connect(m_fileManager, &FileManager::progressChanged, this,
                [&dlgProgress](qint64 cur, qint64 total, const QString &status) {
            if (total > 0)
                dlgProgress.setValue(int(100 * cur / total));
            dlgProgress.setLabelText(status);
        });
        connect(m_fileManager, &FileManager::operationFinished, this,
                [&](bool ok, const QString &msg) {
            dlgProgress.setValue(100);
            dlgProgress.close();
            bOk = ok;
            sResultMsg = msg;
        });
        connect(&dlgProgress, &QProgressDialog::canceled,
                m_fileManager, &FileManager::cancelOperation);

        dlgProgress.show();
        QApplication::processEvents();  // 让进度对话框先绘制出来

        emit QMergeFiles(vtFilesPaths, sMergedFilePath);

        dlgProgress.close();

        if (bOk)
            QMessageBox::information(this, tr("合并文件"), sResultMsg);
        else
            QMessageBox::critical(this, tr("合并文件"), sResultMsg);
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
    QString sPluginName;
    // PluginText 处理的纯文本格式（大小写不敏感），如需新增格式在此添加
    static const QStringList arrTextExts = {
        "txt", "log", "md", "markdown", "csv", "ini", "cfg", "conf",
        "json", "xml", "html", "htm", "css", "js", "jsx", "ts", "tsx",
        "py", "java", "c", "cpp", "cc", "h", "hpp", "cs", "go", "rb",
        "php", "sh", "bat", "ps1", "yml", "yaml", "toml", "sql", "tex", "rst"
    };
    // PluginPicture 处理的图片格式（大小写不敏感），如需新增格式在此添加
    static const QStringList arrPictureExts = {
        "jpg", "jpeg", "png", "bmp", "gif", "xbm", "xpm", "pbm", "pgm", "ppm"
    };
    // PluginMedia 处理的音视频格式（基于 FFmpeg；大小写不敏感），如需新增格式在此添加
    static const QStringList arrMediaExts = {
        // 视频容器
        "mp4", "m4v", "mov", "avi", "wmv", "flv", "mkv", "webm",
        "mpg", "mpeg", "ts", "3gp", "3g2", "ogv", "vob", "asf", "rm", "rmvb",
        // 音频
        "mp3", "wav", "wma", "aac", "m4a", "flac", "ogg", "opus",
        "amr", "aiff", "ape", "ac3"
    };
    // PluginPdf 处理的文档格式（QPdfium 仅支持 PDF，无可扩展项），大小写不敏感
    static const QStringList arrPdfExts = { "pdf" };
    // PluginBook 处理的电子书格式（EPUB/FB2 可解析显示；MOBI/AZW 等暂提示不支持），大小写不敏感
    static const QStringList arrBookExts = {
        "epub", "fb2", "mobi", "azw", "azw3", "prc", "lit"
    };

    if(arrPictureExts.contains(sFileExt, Qt::CaseInsensitive))
    {
        sPluginName = "PluginPicture";
    }
    else if(arrTextExts.contains(sFileExt, Qt::CaseInsensitive))
    {
        sPluginName = "PluginText";
    }
    else if(arrMediaExts.contains(sFileExt, Qt::CaseInsensitive))
    {
        sPluginName = "PluginMedia";
    }
    else if(arrPdfExts.contains(sFileExt, Qt::CaseInsensitive))
    {
        sPluginName = "PluginPdf";
    }
    else if(arrBookExts.contains(sFileExt, Qt::CaseInsensitive))
    {
        sPluginName = "PluginBook";
    }
    else
    {
        return "";
    }
    QString sPluginPath = QString("%1/plugins/%2.dll").arg(qApp->applicationDirPath()).arg(sPluginName);
    return sPluginPath;
}

void MainWindow::on_actionSplitFile_triggered()
{
    DlgSplitFile dlgSplitFile;
    if(dlgSplitFile.exec() == QDialog::Accepted)
    {
        QString sMergedFilePath = dlgSplitFile.getMergedFilePath();

        if(!m_fileManager->validateMergedFile(sMergedFilePath))
        {
            QMessageBox::critical(this, "分割失败", "不是有效的归档文件或文件已损坏。\n"
                                    "（缺少 FWDA 标识、版本不兼容或头部校验失败）");
            return;
        }
        bool bIsSaveAsOldPath = dlgSplitFile.getIsSaveAsOldPath();
        QString sSplitFileDir = "";
        if(!bIsSaveAsOldPath)
        {
            sSplitFileDir = dlgSplitFile.getSplitFileDir();
        }

        emit QSplitFiles(sMergedFilePath, bIsSaveAsOldPath, sSplitFileDir);
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
        QStringList segs = normalizePath(info.sFilePath).split('/', QString::SkipEmptyParts);
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

// 按“归档是否已加载 + 当前展示模式”刷新工具栏动作可用状态
void MainWindow::updateViewActionState()
{
    const bool bLoaded = m_fileManager->isArchiveLoaded();

    m_acViewTree->setEnabled(bLoaded && !m_bTreeMode);
    m_acViewFlat->setEnabled(bLoaded && m_bTreeMode);
    // “显示路径/显示名称”仅在平铺模式下有意义
    m_acShowFilePath->setEnabled(bLoaded && !m_bTreeMode && !m_bShowPath);
    m_acShowFileName->setEnabled(bLoaded && !m_bTreeMode && m_bShowPath);
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
    m_acSaveFile->setEnabled(m_fileManager->isDirty());
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
    m_acSaveFile->setEnabled(m_fileManager->isDirty());
}

void MainWindow::onActionOutputFileTriggered()
{
    // 树形模式下目录节点不可导出，需选中具体文件
    QStandardItem *pItem = m_hModelFilePath->itemFromIndex(m_tvFilePath->currentIndex());
    if (!isFileLeafItem(pItem))
    {
        QMessageBox::information(this, tr("导出文件"),
                                 tr("请先在列表中选择要导出的文件（目录节点不可导出）。"));
        return;
    }
    FILEINFO fileInfo = pItem->data().value<FILEINFO>();

    DlgOutputFile dlgOutputFile(this);
    if(dlgOutputFile.exec() == QDialog::Accepted)
    {
        QString sOutputDir = dlgOutputFile.getOutputDir();
        m_fileManager->outputFile(fileInfo.sFilePath, sOutputDir);
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

    // 清理上一轮可能残留的进度连接，避免 lambda 捕获的局部对话框被悬空引用
    disconnect(m_fileManager, nullptr, this, nullptr);

    connect(m_fileManager, &FileManager::progressChanged, this,
            [&dlgProgress](qint64 cur, qint64 total, const QString &status) {
        if (total > 0)
            dlgProgress.setValue(int(100 * cur / total));
        dlgProgress.setLabelText(status);
    });
    connect(&dlgProgress, &QProgressDialog::canceled,
            m_fileManager, &FileManager::cancelOperation);

    dlgProgress.show();
    QApplication::processEvents();

    bool bOk = m_fileManager->save();

    dlgProgress.close();

    if(!bOk)
    {
        QString sMsg = m_fileManager->isCancelRequested()
                ? tr("保存已取消。")
                : tr("保存失败，归档文件可能已损坏或磁盘写入失败。");
        QMessageBox::critical(this, tr("保存文件"), sMsg);
        return;
    }

    // 保存后刷新列表（重写会更新内存中的内容偏移），并按脏标记更新保存按钮
    refreshFilePathView();
    m_acSaveFile->setEnabled(m_fileManager->isDirty());
}

void MainWindow::on_actionAbout_triggered()
{
    DlgAbout dlgAbout;
    dlgAbout.exec();
}

void MainWindow::on_actionPluginMap_triggered()
{
    QMessageBox::information(this, tr("插件映射"), tr("功能开发中，敬请期待。"));
}

void MainWindow::on_actionUnloadFile_triggered()
{
    if (!confirmDiscardOrSaveIfDirty())
        return;

    m_fileManager->unLoadMergedFile();
    m_hModelFilePath->removeRows(0, m_hModelFilePath->rowCount());
    m_tvFilePath->header()->setVisible(false);
    m_acSaveFile->setEnabled(false);
    updateViewActionState();

    // 卸载归档后关闭所有已打开的文件标签页
    closeAllTabs();
}

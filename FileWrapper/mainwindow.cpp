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

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    initWidgetFilePath();
    initWidgetFileContent();

    m_fileManager = new FileManager;

    // 启动时一次性加载所有插件（避免每次双击重复 reload）
    PluginManager::getInstance()->loadAllPlugins();

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
    m_acShowFilePath = new QAction("显示路径", m_tbFilePath);
    m_acShowFileName = new QAction("显示名称", m_tbFilePath);
    m_acAddFile = new QAction("添加文件", m_tbFilePath);
    m_acDelFile = new QAction("删除文件", m_tbFilePath);
    m_acOutputFile = new QAction("导出文件", m_tbFilePath);
    m_acSaveFile = new QAction("保存文件", m_tbFilePath);
    m_acSaveFile->setEnabled(false);

    m_tbFilePath->addAction(m_acShowFilePath);
    m_tbFilePath->addAction(m_acShowFileName);
    m_tbFilePath->addAction(m_acAddFile);
    m_tbFilePath->addAction(m_acDelFile);
    m_tbFilePath->addAction(m_acOutputFile);
    m_tbFilePath->addAction(m_acSaveFile);

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

    m_acShowFileName->setEnabled(false);
}

// 初始化【文件内容】窗口
void MainWindow::initWidgetFileContent()
{
//    m_swShowArea = ui->stackedWidget;
}

void MainWindow::on_treeView_doubleClicked(const QModelIndex &index)
{
    FILEINFO fInfo = m_hModelFilePath->itemFromIndex(index)->data().value<FILEINFO>();
    QString sFilePath(fInfo.sFilePath);
    QFileInfo fileInfo(sFilePath);
    QString sExt = fileInfo.suffix();

    char *szBuf = nullptr;
    qint64 nFileLen = 0;
    m_fileManager->QGetFileContent(sFilePath, &szBuf, nFileLen);
    if(szBuf == nullptr)
    {
        qDebug()<<"获取文件内容失败:"<<sFilePath;
        return;
    }

    PluginInterface* pInterface = nullptr;

    QString sPluginPath = choosePlugin(sExt);
    if(sPluginPath.isEmpty())
    {
        QMessageBox::information(this, "", "没有插件来处理该类型文件，请手动映射插件");
        delete[] szBuf;
        return;
    }
    pInterface = PluginManager::getInstance()->getInterface(sPluginPath);

    // Fix: getInterface 可能返回 nullptr（插件加载失败但元数据已注册时）
    if (!pInterface)
    {
        qDebug() << "获取插件接口失败:" << sPluginPath;
        delete[] szBuf;
        return;
    }

    QWidget *wPlugin = pInterface->getPluginWidget();

    int nCurIndex = ui->tabWidget->currentIndex();

    // Fix: 已是目标 widget 则直接复用，避免无意义的 remove/add
    if (nCurIndex != -1 && ui->tabWidget->widget(nCurIndex) == wPlugin)
    {
        pInterface->sendFileData(szBuf, nFileLen);
        delete[] szBuf;
        return;
    }

    if (nCurIndex != -1)
    {
        ui->tabWidget->removeTab(nCurIndex);
        // 注意：旧 page widget 不删除，它由其 Plugin 实例持有生命周期
    }
    ui->tabWidget->addTab(wPlugin, "");

    // 插件在 sendFileData 返回前会同步复制/使用数据，因此返回后可安全释放
    pInterface->sendFileData(szBuf, nFileLen);
    delete[] szBuf;
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
        m_acShowFileName->setEnabled(false);
        m_acShowFilePath->setEnabled(false);
        QMessageBox::critical(this, "加载失败", "不是有效的归档文件或文件已损坏。\n"
                                "（缺少 FWDA 标识、版本不兼容或头部校验失败）");
        return;
    }

    m_hModelFilePath->setHeaderData(0, Qt::Horizontal, sFileName);
    m_tvFilePath->header()->setVisible(true);

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
    if(QString::compare(sFileExt, "jpg", Qt::CaseInsensitive) == 0)
    {
        sPluginName = "PluginPicture";
    }
    else if(QString::compare(sFileExt, "txt", Qt::CaseInsensitive) == 0 ||
            QString::compare(sFileExt, "log", Qt::CaseInsensitive) == 0)
    {
        sPluginName = "PluginText";
    }
    else if(QString::compare(sFileExt, "mp4", Qt::CaseInsensitive) == 0 ||
            QString::compare(sFileExt, "wmv", Qt::CaseInsensitive) == 0 ||
            QString::compare(sFileExt, "mp3", Qt::CaseInsensitive) == 0 ||
            QString::compare(sFileExt, "wav", Qt::CaseInsensitive) == 0)
    {
        sPluginName = "PluginMedia";
    }
    else if(QString::compare(sFileExt, "pdf", Qt::CaseInsensitive) == 0)
    {
        sPluginName = "PluginPdf";
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

void MainWindow::populateFilePathView(bool bShowPath)
{
    QVector<FILEINFO> vtFileInfos = m_fileManager->getFileInfos();
    int nRow = 0;
    m_hModelFilePath->removeRows(0, m_hModelFilePath->rowCount());
    for(QVector<FILEINFO>::const_iterator cIt = vtFileInfos.begin(); cIt != vtFileInfos.end(); cIt++, nRow++)
    {
        QStandardItem* hItemName = new QStandardItem(bShowPath ? (*cIt).sFilePath : (*cIt).sFileName);
        QVariant var;
        var.setValue(*cIt);
        hItemName->setData(var);
        m_hModelFilePath->setItem(nRow, 0, hItemName);
    }
    m_bShowPath = bShowPath;
    m_acShowFileName->setEnabled(bShowPath);
    m_acShowFilePath->setEnabled(!bShowPath);
}

void MainWindow::refreshFilePathView()
{
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

    int nCurRow = m_tvFilePath->currentIndex().row();
    if(nCurRow == -1)
    {
        QMessageBox::information(this, tr("删除文件"),
                                 tr("请先在列表中选择要删除的文件。"));
        return;
    }

    FILEINFO fileInfo = m_hModelFilePath->item(nCurRow)->data().value<FILEINFO>();

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
    int nCurRow = m_tvFilePath->currentIndex().row();
    if(nCurRow == -1)
    {
        qDebug()<<"未选中文件";
        return;
    }
    FILEINFO fileInfo = m_hModelFilePath->item(nCurRow)->data().value<FILEINFO>();

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
}

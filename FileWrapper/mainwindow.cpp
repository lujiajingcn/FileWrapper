#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QLabel>
#include <QFileDialog>
#include "dlgmergefile.h"
#include "dlgsplitfile.h"
#include "pluginmanager.h"
#include <QDebug>
#include "dlgabout.h"
#include <QVariant>
#include "dlgoutputfile.h"
#include <QMessageBox>

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    initWidgetFilePath();
    initWidgetFileContent();

    m_fileManager = new FileManager;

//    connect(this, &MainWindow::mergeFiles, m_fileManager, &FileManager::mergeFiles);
//    connect(this, &MainWindow::splitFiles, m_fileManager, &FileManager::splitFiles);

    connect(this, &MainWindow::QMergeFiles, m_fileManager, &FileManager::QMergeFiles);
    connect(this, &MainWindow::QSplitFiles, m_fileManager, &FileManager::QSplitFiles);
}

MainWindow::~MainWindow()
{
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
#include <QThread>
void MainWindow::on_treeView_doubleClicked(const QModelIndex &index)
{
    FILEINFO fInfo = m_hModelFilePath->itemFromIndex(index)->data().value<FILEINFO>();
    QString sFilePath(fInfo.sFilePath);
    QFileInfo fileInfo(sFilePath);
    QString sExt = fileInfo.suffix();

    char *szBuf = nullptr;
    int nFileLen = 0;
    m_fileManager->QGetFileContent(sFilePath, &szBuf, nFileLen);

    PluginManager::getInstance()->loadAllPlugins();
    PluginInterface* pInterface = nullptr;

    QString sPluginPath = choosePlugin(sExt);
    if(sPluginPath.isEmpty())
    {
        QMessageBox().information(nullptr, "", "没有插件来处理该类型文件，请手动映射插件");
        return;
    }
    pInterface = PluginManager::getInstance()->getInterface(sPluginPath);

    QWidget *wPlugin = pInterface->getPluginWidget();

    int nCurIndex = ui->tabWidget->currentIndex();
    if(nCurIndex != -1)
    {
        ui->tabWidget->removeTab(nCurIndex);
    }
    ui->tabWidget->addTab(wPlugin, "");

    pInterface->sendFileData(szBuf, nFileLen);

}

void MainWindow::on_actionLoadFile_triggered()
{
    QFileDialog fileDialog;
    QString sFileName = QFileDialog::getOpenFileName(this, tr("加载文件"), "", tr("Class Files (*.dat);;All Files (*.*)"));
    if(sFileName.isEmpty())
        return;

    m_fileManager->QLoadMergedFile(sFileName);

    m_hModelFilePath->setHeaderData(0, Qt::Horizontal, sFileName);
    m_tvFilePath->header()->setVisible(true);

    QVector<FILEINFO> vtFileInfos = m_fileManager->getFileInfos();
    int nRow = 0;
    m_hModelFilePath->removeRows(0, m_hModelFilePath->rowCount());
    for(QVector<FILEINFO>::const_iterator cIt = vtFileInfos.begin(); cIt != vtFileInfos.end(); cIt++, nRow++)
    {
        QStandardItem* hItemName = new QStandardItem((*cIt).sFileName);
        QVariant var;
        var.setValue(*cIt);
        hItemName->setData(var);
        m_hModelFilePath->setItem(nRow, 0, hItemName);
    }
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

        emit QMergeFiles(vtFilesPaths, sMergedFilePath);
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
            QString::compare(sFileExt, "wmv", Qt::CaseInsensitive) == 0)
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
        bool bIsSaveAsOldPath = dlgSplitFile.getIsSaveAsOldPath();
        QString sSplitFileDir = "";
        if(!bIsSaveAsOldPath)
        {
            sSplitFileDir = dlgSplitFile.getSplitFileDir();
        }
        else
        {
            emit QSplitFiles(sMergedFilePath, true, "");
        }
    }
}

void MainWindow::onActionShowFilePathTriggered()
{
    QVector<FILEINFO> vtFileInfos = m_fileManager->getFileInfos();
    int nRow = 0;
    m_hModelFilePath->removeRows(0, m_hModelFilePath->rowCount());
    for(QVector<FILEINFO>::const_iterator cIt = vtFileInfos.begin(); cIt != vtFileInfos.end(); cIt++, nRow++)
    {
        QStandardItem* hItemName = new QStandardItem((*cIt).sFilePath);
        QVariant var;
        var.setValue(*cIt);
        hItemName->setData(var);
        m_hModelFilePath->setItem(nRow, 0, hItemName);
    }
    m_acShowFileName->setEnabled(true);
    m_acShowFilePath->setEnabled(false);
}

void MainWindow::onActionShowFileNameTriggered()
{
    QVector<FILEINFO> vtFileInfos = m_fileManager->getFileInfos();
    int nRow = 0;
    m_hModelFilePath->removeRows(0, m_hModelFilePath->rowCount());
    for(QVector<FILEINFO>::const_iterator cIt = vtFileInfos.begin(); cIt != vtFileInfos.end(); cIt++, nRow++)
    {
        QStandardItem* hItemName = new QStandardItem((*cIt).sFileName);
        QVariant var;
        var.setValue(*cIt);
        hItemName->setData(var);
        m_hModelFilePath->setItem(nRow, 0, hItemName);
    }
    m_acShowFileName->setEnabled(false);
    m_acShowFilePath->setEnabled(true);
}

void MainWindow::onActionAddFileTriggered()
{

}

void MainWindow::onActionDelFileTriggered()
{

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

}

void MainWindow::on_actionAbout_triggered()
{
    DlgAbout dlgAbout;
    dlgAbout.exec();
}

void MainWindow::on_actionPluginMap_triggered()
{

}

void MainWindow::on_actionUnloadFile_triggered()
{
    m_fileManager->unLoadMergedFile();
    m_hModelFilePath->removeRows(0, m_hModelFilePath->rowCount());
    m_tvFilePath->header()->setVisible(false);
}

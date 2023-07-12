#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QStandardItemModel>
#include "filemanager.h"
#include <QTreeView>
#include <QStackedWidget>
#include <QHeaderView>

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

signals:
    void mergeFiles(vector<string> inputFiles, string sMergedFilePath);
    void splitFiles(string sFilePath, bool bIsSaveAsOldPath, string sSplitFileDir);
    void QMergeFiles(QVector<QString> vtInputFiles, QString sOutputFile);
    void QSplitFiles(QString sInputFile, bool bIsSaveAsOldPath, QString sSplitFileDir);

private slots:
    void on_treeView_doubleClicked(const QModelIndex &index);

    void on_actionLoadFile_triggered();

    void on_actionMergeFile_triggered();

    void on_actionSplitFile_triggered();
private slots:
    void onActionShowFilePathTriggered();
    void onActionShowFileNameTriggered();
    void onActionAddFileTriggered();
    void onActionDelFileTriggered();
    void onActionOutputFileTriggered();
    void onActionSaveFileTriggered();

    void on_actionAbout_triggered();

    void on_actionPluginMap_triggered();

    void on_actionUnloadFile_triggered();

protected:
    bool FindFile(const QString sDir, QStringList& arrFileExts, QStringList& arrFiles, bool bSubDir);
    bool CheckFileExt(QString sFileName, QStringList& arrFileExts);
    QString choosePlugin(QString sFileExt);
private:
    QStandardItemModel      *m_hModelFilePath;
    FileManager             *m_fileManager;

    QStackedWidget          *m_swShowArea;
    QTreeView               *m_tvFilePath;      // 文件路径列表
    QToolBar                *m_tbFilePath;      // 文件路径列表的工具栏
    QAction                 *m_acShowFilePath;
    QAction                 *m_acShowFileName;
    QAction                 *m_acAddFile;
    QAction                 *m_acDelFile;
    QAction                 *m_acOutputFile;
    QAction                 *m_acSaveFile;

    Ui::MainWindow *ui;
};

#endif // MAINWINDOW_H

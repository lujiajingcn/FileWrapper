#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QStandardItemModel>
#include "filemanager.h"
#include <QTreeView>
#include <QStackedWidget>
#include <QHeaderView>
#include <QToolBar>
#include <QAction>

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

    // 按当前模式（路径/名称）刷新文件列表视图
    void populateFilePathView(bool bShowPath);
    void refreshFilePathView();

    // 加载新归档 / 卸载前，若有未保存改动则提示“保存 / 不保存 / 取消”。
    // 返回 true 表示可继续（无改动、已保存成功、或用户选择不保存）；
    // 返回 false 表示用户取消，调用方应中止操作。
    bool confirmDiscardOrSaveIfDirty();
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

    bool                    m_bShowPath = false;  // 当前列表显示模式：true=显示路径, false=显示名称

    Ui::MainWindow *ui;
};

#endif // MAINWINDOW_H

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
#include <QVector>

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
    void onActionViewTreeTriggered();
    void onActionViewFlatTriggered();
    void onActionShowFilePathTriggered();
    void onActionShowFileNameTriggered();
    void onActionAddFileTriggered();
    void onActionDelFileTriggered();
    void onActionOutputFileTriggered();
    void onActionSaveFileTriggered();

    void on_actionAbout_triggered();

    void on_actionPluginMap_triggered();

    void on_actionUnloadFile_triggered();

    // —— 多标签页支持 ——
    void onTabCloseRequested(int index);          // 用户点击标签页关闭按钮
    void onTabCurrentChanged(int index);          // 切换标签页
    void onTreeViewCustomContextMenu(const QPoint &pos); // 文件列表右键菜单
    void onActionNewTabTriggered();               // 标签栏“+”新建空白标签页

protected:
    bool FindFile(const QString sDir, QStringList& arrFileExts, QStringList& arrFiles, bool bSubDir);
    bool CheckFileExt(QString sFileName, QStringList& arrFileExts);
    QString choosePlugin(QString sFileExt);

    // 按当前模式（路径/名称）刷新文件列表视图
    void populateFilePathView(bool bShowPath);
    void refreshFilePathView();

    // 平铺列表：每个文件一行，文本为完整路径或文件名
    void populateFilePathList(bool bShowPath);
    // 树形列表：按合并前的目录层级还原为父子结构（根为所有条目的公共父目录）
    void populateFilePathTree();

    // 按“归档是否已加载 + 当前展示模式”刷新工具栏各动作的可用状态
    void updateViewActionState();

    // 重建视图前后保存 / 恢复各目录节点的展开状态（键为“从根到该节点的文本链”）
    void collectExpandedItems(const QModelIndex &index, const QString &sKeyPrefix, QSet<QString> &setExpanded);
    void restoreExpandedItems(const QModelIndex &index, const QString &sKeyPrefix, const QSet<QString> &setExpanded);

    // 加载新归档 / 卸载前，若有未保存改动则提示“保存 / 不保存 / 取消”。
    // 返回 true 表示可继续（无改动、已保存成功、或用户选择不保存）；
    // 返回 false 表示用户取消，调用方应中止操作。
    bool confirmDiscardOrSaveIfDirty();
private:
    // 一个标签页对应一个“已打开的文档”。
    // 注意：每种插件类型只有唯一一个共享 widget（由插件自身持有生命周期），
    // 因此同一时刻只有一个标签页能“挂载”某类型的插件 widget；切换标签页时
    // 该 widget 在标签页之间迁移，非激活标签页显示为占位；切换回来时重新渲染。
    struct TabDoc {
        bool             hasFile = false;  // 是否为已打开文件的标签页（false=空白标签页）
        FILEINFO         info;             // 记录项文件信息
        QString          pluginPath;       // 处理该文件的插件路径
        PluginInterface *interface = nullptr;
    };

    QStandardItemModel      *m_hModelFilePath;
    FileManager             *m_fileManager;

    QStackedWidget          *m_swShowArea;
    QTreeView               *m_tvFilePath;      // 文件路径列表
    QToolBar                *m_tbFilePath;      // 文件路径列表的工具栏
    QAction                 *m_acViewTree;
    QAction                 *m_acViewFlat;
    QAction                 *m_acShowFilePath;
    QAction                 *m_acShowFileName;
    QAction                 *m_acAddFile;
    QAction                 *m_acDelFile;
    QAction                 *m_acOutputFile;
    QAction                 *m_acSaveFile;

    bool                    m_bTreeMode = true;   // 列表展示模式：true=按原始路径层级树形展示, false=平铺
    bool                    m_bShowPath = false;  // 平铺模式下列表文本：true=显示路径, false=显示名称

    PluginInterface         *m_pCurrentInterface = nullptr;  // 当前展示的插件接口，切换文件时用于停止上一插件的后台播放

    // —— 多标签页相关成员 ——
    QVector<TabDoc>          m_vTabDocs;          // 与 tabWidget 各页一一对应的文档描述
    QSet<QWidget*>           m_setPluginWidgets;  // 所有插件 widget 集合（关闭标签页时据此避免误删）
    QWidget                 *m_pMountedWidget = nullptr; // 当前挂载在激活标签页中的插件 widget

    void initTabWidget();                         // 初始化 tabWidget（清空默认页、可关闭、新建按钮、信号连接）
    void openFileInNewTab(const FILEINFO& info);  // 新建标签页并打开文件
    void openFileInCurrentTab(const FILEINFO& info); // 在当前激活标签页打开文件
    void activateTab(int index);                 // 激活某标签页：迁移并挂载插件 widget、渲染文件
    void renderFile(const FILEINFO& info, PluginInterface* iface); // 读取文件内容并送入插件
    void closeAllTabs();                         // 关闭全部标签页（用于加载/卸载归档）
    void newEmptyTab();                          // 新建一个空白标签页
    bool resolvePlugin(const FILEINFO &info, QString &sPluginPath, PluginInterface* &pInterface); // 选择并解析插件接口

    Ui::MainWindow *ui;
};

#endif // MAINWINDOW_H

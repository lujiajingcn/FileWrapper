#ifndef DLGPLUGINMAP_H
#define DLGPLUGINMAP_H

#include <QDialog>
#include <QString>
#include <QStringList>

class QTableWidget;
class QLineEdit;
class QComboBox;
class QPushButton;

// 后缀 → 插件映射对话框；映射持久化在 QSettings（P2-2）
class DlgPluginMap : public QDialog
{
public:
    explicit DlgPluginMap(QWidget *parent = nullptr);

    // 供 MainWindow::choosePlugin 查询 / 写入映射
    static QString mappingFor(const QString &sExt);
    static void setMapping(const QString &sExt, const QString &sPluginName);
    static void removeMapping(const QString &sExt);
    static QStringList allMappedExts();
    static QStringList availablePluginNames();

private:
    void reloadTable();
    void onAdd();
    void onRemoveSelected();
    void updateButtonStates();

    QTableWidget *m_table;
    QLineEdit    *m_ext;
    QComboBox    *m_plugin;
    QPushButton  *m_btnAdd = nullptr;
    QPushButton  *m_btnDel = nullptr;
};

#endif // DLGPLUGINMAP_H

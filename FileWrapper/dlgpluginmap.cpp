#include "dlgpluginmap.h"
#include "pluginmanager.h"

#include <QTableWidget>
#include <QTableWidgetItem>
#include <QAbstractItemView>
#include <QHeaderView>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSettings>
#include <QDir>
#include <QApplication>
#include <QLibrary>
#include <QMessageBox>
#include <QFileInfo>

static const char *kGroup = "PluginMap";

QStringList DlgPluginMap::availablePluginNames()
{
    // 插件逻辑名以 PluginManager 的注册表为准（C1 起含 sidecar 里的信息）：
    // 即使某个插件当前加载失败，"插件设置 → 插件映射" 里仍然能看到并指定它。
    QStringList names = PluginManager::getInstance()->knownPluginIds();

    if (names.isEmpty())
    {
        // 兜底：插件管理器尚未扫描过（异常启动路径）时退回目录扫描
        QDir dir(QApplication::applicationDirPath() + QStringLiteral("/plugins"));
        if (dir.exists())
        {
            const QStringList files = dir.entryList(QDir::Files);
            for (const QString &f : files)
            {
                if (!QLibrary::isLibrary(f)) continue;
                QString base = QFileInfo(f).completeBaseName();
                if (base.startsWith(QStringLiteral("lib"), Qt::CaseInsensitive)) base.remove(0, 3);
                names << base;
            }
        }
    }

    names << QStringLiteral("builtin://text") << QStringLiteral("builtin://hex");
    names.removeDuplicates();
    names.sort(Qt::CaseInsensitive);
    return names;
}

QString DlgPluginMap::mappingFor(const QString &sExt)
{
    if (sExt.isEmpty()) return QString();
    QSettings st(QSettings::IniFormat, QSettings::UserScope,
                 QStringLiteral("FileWrapper"), QStringLiteral("FileWrapper"));
    st.beginGroup(QString::fromLatin1(kGroup));
    return st.value(sExt.toLower(), QString()).toString();
}

void DlgPluginMap::setMapping(const QString &sExt, const QString &sPluginName)
{
    if (sExt.isEmpty()) return;
    QSettings st(QSettings::IniFormat, QSettings::UserScope,
                 QStringLiteral("FileWrapper"), QStringLiteral("FileWrapper"));
    st.beginGroup(QString::fromLatin1(kGroup));
    st.setValue(sExt.toLower(), sPluginName);
    st.sync();
}

void DlgPluginMap::removeMapping(const QString &sExt)
{
    QSettings st(QSettings::IniFormat, QSettings::UserScope,
                 QStringLiteral("FileWrapper"), QStringLiteral("FileWrapper"));
    st.beginGroup(QString::fromLatin1(kGroup));
    st.remove(sExt.toLower());
    st.sync();
}

QStringList DlgPluginMap::allMappedExts()
{
    QSettings st(QSettings::IniFormat, QSettings::UserScope,
                 QStringLiteral("FileWrapper"), QStringLiteral("FileWrapper"));
    st.beginGroup(QString::fromLatin1(kGroup));
    return st.allKeys();
}

DlgPluginMap::DlgPluginMap(QWidget *parent)
    : QDialog(parent), m_table(nullptr), m_ext(nullptr), m_plugin(nullptr)
{
    setWindowTitle(QStringLiteral("插件映射"));
    resize(560, 420);

    auto *root = new QVBoxLayout(this);
    root->addWidget(new QLabel(QStringLiteral("为文件后缀指定处理插件；映射会保存在本机配置中。"), this));

    m_table = new QTableWidget(0, 2, this);
    m_table->setHorizontalHeaderLabels(QStringList() << QStringLiteral("后缀") << QStringLiteral("插件"));
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    root->addWidget(m_table, 1);

    auto *row = new QHBoxLayout;
    m_ext = new QLineEdit(this);
    m_ext->setPlaceholderText(QStringLiteral("例如 iso"));
    m_ext->setMaximumWidth(140);
    m_plugin = new QComboBox(this);
    m_plugin->addItems(availablePluginNames());
    auto *btnAdd = new QPushButton(QStringLiteral("添加 / 更新"), this);
    auto *btnDel = new QPushButton(QStringLiteral("删除选中"), this);
    row->addWidget(m_ext);
    row->addWidget(m_plugin, 1);
    row->addWidget(btnAdd);
    row->addWidget(btnDel);
    root->addLayout(row);

    auto *bottom = new QHBoxLayout;
    auto *btnClose = new QPushButton(QStringLiteral("关闭"), this);
    bottom->addStretch(1);
    bottom->addWidget(btnClose);
    root->addLayout(bottom);

    connect(btnAdd, &QPushButton::clicked, this, [this]() { onAdd(); });
    connect(btnDel, &QPushButton::clicked, this, [this]() { onRemoveSelected(); });
    connect(btnClose, &QPushButton::clicked, this, [this]() { accept(); });
    connect(m_table, &QTableWidget::cellClicked, this, [this](int r, int) {
        if (r < 0 || r >= m_table->rowCount()) return;
        if (m_table->item(r, 0)) m_ext->setText(m_table->item(r, 0)->text());
        if (m_table->item(r, 1))
        {
            const int idx = m_plugin->findText(m_table->item(r, 1)->text());
            if (idx >= 0) m_plugin->setCurrentIndex(idx);
        }
    });

    reloadTable();
}

void DlgPluginMap::reloadTable()
{
    const QStringList exts = allMappedExts();
    m_table->setRowCount(exts.size());
    for (int i = 0; i < exts.size(); ++i)
    {
        m_table->setItem(i, 0, new QTableWidgetItem(exts.at(i)));
        m_table->setItem(i, 1, new QTableWidgetItem(mappingFor(exts.at(i))));
    }
    m_table->resizeColumnToContents(0);
}

void DlgPluginMap::onAdd()
{
    QString ext = m_ext->text().trimmed();
    while (ext.startsWith('.')) ext.remove(0, 1);
    if (ext.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("插件映射"), QStringLiteral("请先填写后缀。"));
        return;
    }
    if (ext.contains('/') || ext.contains('\\') || ext.contains(' '))
    {
        QMessageBox::information(this, QStringLiteral("插件映射"), QStringLiteral("后缀不能包含路径分隔符或空格。"));
        return;
    }
    setMapping(ext, m_plugin->currentText());
    reloadTable();
}

void DlgPluginMap::onRemoveSelected()
{
    const int r = m_table->currentRow();
    if (r < 0 || !m_table->item(r, 0)) return;
    removeMapping(m_table->item(r, 0)->text());
    reloadTable();
}

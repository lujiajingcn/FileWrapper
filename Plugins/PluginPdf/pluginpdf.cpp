#include "pluginpdf.h"
#include <QFileInfo>

PluginPdf::PluginPdf(QObject *parent) : QObject(parent)
{
    m_wMainWindow = new MainWindow;
}

PluginPdf::~PluginPdf()
{
    delete m_wMainWindow;
    m_wMainWindow = nullptr;
}

void PluginPdf::sendFileData(char *szFileData, qint64 nFileLen)
{
    if (szFileData == nullptr || nFileLen <= 0)
    {
        // 主程序在读文件失败 / 0 字节时也会送 (nullptr, 0)：必须清掉内容区，
        // 否则界面仍停留在上一个文件。
        if (m_wMainWindow) m_wMainWindow->clearContent();
        return;
    }
    m_wMainWindow->showPdf(szFileData, nFileLen);
}

QWidget* PluginPdf::getPluginWidget()
{
    return m_wMainWindow;
}

PluginInterface* PluginPdf::createInstance()
{
    // 为新标签页创建一个独立实例（含独立的内部窗口），使同类型多标签页各自独立、互不干扰
    return new PluginPdf;
}

bool PluginPdf::prefersFileByPath() const
{
    return true;
}

bool PluginPdf::sendFileByPath(const QString &path, const QString &displayName)
{
    Q_UNUSED(displayName);
    if (path.isEmpty() || !QFileInfo::exists(path) || m_wMainWindow == nullptr)
        return false;
    return m_wMainWindow->showPdfFile(path);
}

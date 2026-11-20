#include "pluginpdf.h"

#include <QFileInfo>

bool PluginPdf::showFile(const PluginFile &file)
{
    if (m_wMainWindow == nullptr)
        return false;
    m_wMainWindow->clearLastError();   // 不回报上一次打开留下的陈旧原因

    if (file.isPath())
    {
        if (file.path().isEmpty() || !QFileInfo::exists(file.path()))
            return false;
        return m_wMainWindow->showPdfFile(file.path());
    }

    const QByteArray &data = file.memory();
    if (data.isEmpty())
        return false;
    return m_wMainWindow->showPdf(const_cast<char *>(data.constData()), data.size());
}

QString PluginPdf::lastError() const
{
    return m_wMainWindow ? m_wMainWindow->lastError() : QString();
}

bool PluginPdf::prefersFileByPath() const
{
    return true;
}

bool PluginPdf::sendFileByPath(const QString &path, const QString &displayName)
{
    Q_UNUSED(displayName);
    if (m_wMainWindow == nullptr)
        return false;
    m_wMainWindow->clearLastError();   // 同上：每次打开尝试都从"没有原因"开始
    if (path.isEmpty() || !QFileInfo::exists(path))
        return false;
    return m_wMainWindow->showPdfFile(path);
}

#include "pluginpdf.h"

#include <QFileInfo>

bool PluginPdf::showFile(const PluginFile &file)
{
    if (file.isPath())
    {
        if (file.path().isEmpty() || !QFileInfo::exists(file.path()))
            return false;
        return m_wMainWindow->showPdfFile(file.path());
    }

    const QByteArray &data = file.memory();
    if (data.isEmpty())
        return false;
    m_wMainWindow->showPdf(const_cast<char *>(data.constData()), data.size());
    return true;
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

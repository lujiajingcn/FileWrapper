#include "pluginsheet.h"

bool PluginSheet::showFile(const PluginFile &file)
{
    if (file.isPath())
    {
        const QByteArray data = file.readAll();
        if (data.isEmpty())
            return false;
        m_wMainWindow->showSheet(data);
        return true;
    }

    const QByteArray &data = file.memory();
    if (data.isEmpty())
        return false;
    m_wMainWindow->showSheet(data);
    return true;
}

void PluginSheet::setSourceName(const QString &name)
{
    if (m_wMainWindow) m_wMainWindow->setSourceName(name);
}

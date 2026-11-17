#include "pluginslides.h"

bool PluginSlides::showFile(const PluginFile &file)
{
    if (file.isPath())
    {
        const QByteArray data = file.readAll();
        if (data.isEmpty())
            return false;
        m_wMainWindow->showSlides(data);
        return true;
    }

    const QByteArray &data = file.memory();
    if (data.isEmpty())
        return false;
    m_wMainWindow->showSlides(data);
    return true;
}

void PluginSlides::setSourceName(const QString &name)
{
    if (m_wMainWindow) m_wMainWindow->setSourceName(name);
}

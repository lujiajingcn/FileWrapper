#include "pluginpicture.h"

bool PluginPicture::showFile(const PluginFile &file)
{
    if (file.isPath())
    {
        const QByteArray data = file.readAll();
        if (data.isEmpty())
            return false;
        m_wMainWindow->showPicture(const_cast<char *>(data.constData()), data.size());
        return true;
    }

    const QByteArray &data = file.memory();
    if (data.isEmpty())
        return false;
    m_wMainWindow->showPicture(const_cast<char *>(data.constData()), data.size());
    return true;
}

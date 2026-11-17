#include "pluginbook.h"

bool PluginBook::showFile(const PluginFile &file)
{
    if (file.isPath())
    {
        const QByteArray data = file.readAll();
        if (data.isEmpty())
            return false;
        m_wMainWindow->showBook(const_cast<char *>(data.constData()), data.size());
        return true;
    }

    const QByteArray &data = file.memory();
    if (data.isEmpty())
        return false;
    // showBook 内部会复制一份并持有（m_bookData），这里传只读指针安全
    m_wMainWindow->showBook(const_cast<char *>(data.constData()), data.size());
    return true;
}

void PluginBook::setSourceName(const QString &name)
{
    if (m_wMainWindow) m_wMainWindow->setSourceName(name);
}

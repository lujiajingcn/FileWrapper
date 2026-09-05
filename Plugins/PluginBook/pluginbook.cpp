#include "pluginbook.h"

PluginBook::PluginBook(QObject *parent) : QObject(parent)
{
    m_wMainWindow = new MainWindow;
}

PluginBook::~PluginBook()
{
    delete m_wMainWindow;
    m_wMainWindow = nullptr;
}

void PluginBook::sendFileData(char *szFileData, qint64 nFileLen)
{
    if (szFileData == nullptr || nFileLen <= 0)
        return;
    m_wMainWindow->showBook(szFileData, nFileLen);
}

QWidget* PluginBook::getPluginWidget()
{
    return m_wMainWindow;
}

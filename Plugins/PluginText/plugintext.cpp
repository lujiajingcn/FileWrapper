#include "plugintext.h"

PluginText::PluginText(QObject *parent) : QObject(parent)
{
    m_wMainWindow = new MainWindow;
}

PluginText::~PluginText()
{
    delete m_wMainWindow;
    m_wMainWindow = nullptr;
}

void PluginText::sendFileData(char *szFileData, qint64 nFileLen)
{
    if (szFileData == nullptr || nFileLen <= 0)
        return;
    m_wMainWindow->showText(szFileData, nFileLen);
}

QWidget* PluginText::getPluginWidget()
{
    return m_wMainWindow;
}

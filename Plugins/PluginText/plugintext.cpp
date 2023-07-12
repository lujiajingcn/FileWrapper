#include "plugintext.h"

PluginText::PluginText(QObject *parent) : QObject(parent)
{
    m_wMainWindow = new MainWindow;
}

void PluginText::sendFileData(char *szFileData, int nFileLen)
{
    m_wMainWindow->showText(szFileData, nFileLen);
}

QWidget* PluginText::getPluginWidget()
{
    return m_wMainWindow;
}

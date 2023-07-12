#include "pluginpicture.h"
#include <QDebug>

PluginPicture::PluginPicture(QObject *parent) : QObject(parent)
{
    m_wMainWindow = new MainWindow;
}

void PluginPicture::sendFileData(char *szFileData, int nFileLen)
{
    m_wMainWindow->showPicture(szFileData, nFileLen);
}

QWidget* PluginPicture::getPluginWidget()
{
    return m_wMainWindow;
}

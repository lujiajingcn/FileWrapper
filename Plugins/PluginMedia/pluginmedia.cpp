#include "pluginmedia.h"

PluginMedia::PluginMedia(QObject *parent) : QObject(parent)
{
    m_wMainWindow = new MainWindow;
    connect(this, &PluginMedia::showVideo, m_wMainWindow, &MainWindow::showVideo);
}

void PluginMedia::sendFileData(char *szFileData, int nFileLen)
{
    emit showVideo(szFileData, nFileLen);
}

QWidget* PluginMedia::getPluginWidget()
{
    return m_wMainWindow;
}

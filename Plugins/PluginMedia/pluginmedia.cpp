#include "pluginmedia.h"

PluginMedia::PluginMedia(QObject *parent) : QObject(parent)
{
    m_wMainWindow = new MainWindow;
    connect(this, &PluginMedia::showVideo, m_wMainWindow, &MainWindow::showVideo);
}

PluginMedia::~PluginMedia()
{
    delete m_wMainWindow;
    m_wMainWindow = nullptr;
}

void PluginMedia::sendFileData(char *szFileData, qint64 nFileLen)
{
    if (szFileData == nullptr || nFileLen <= 0)
        return;
    emit showVideo(szFileData, nFileLen);
}

QWidget* PluginMedia::getPluginWidget()
{
    return m_wMainWindow;
}

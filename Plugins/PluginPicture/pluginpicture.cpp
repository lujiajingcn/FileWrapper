#include "pluginpicture.h"
#include <QDebug>

PluginPicture::PluginPicture(QObject *parent) : QObject(parent)
{
    m_wMainWindow = new MainWindow;
}

PluginPicture::~PluginPicture()
{
    delete m_wMainWindow;
    m_wMainWindow = nullptr;
}

void PluginPicture::sendFileData(char *szFileData, qint64 nFileLen)
{
    if (szFileData == nullptr || nFileLen <= 0)
        return;
    m_wMainWindow->showPicture(szFileData, nFileLen);
}

QWidget* PluginPicture::getPluginWidget()
{
    return m_wMainWindow;
}

PluginInterface* PluginPicture::createInstance()
{
    // 为新标签页创建一个独立实例（含独立的内部窗口），使同类型多标签页各自独立、互不干扰
    return new PluginPicture;
}

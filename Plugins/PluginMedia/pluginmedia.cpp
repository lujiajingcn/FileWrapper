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

void PluginMedia::stopPlayback()
{
    if (m_wMainWindow)
        m_wMainWindow->stopPlayback();
}

bool PluginMedia::hasBackgroundPlayback() const
{
    // 音视频插件存在后台解码线程与音频输出：被主程序 stopPlayback() 停止后，
    // 其所在标签页再次激活时需要重新渲染（重新打开文件），故返回 true。
    return true;
}

PluginInterface* PluginMedia::createInstance()
{
    // 为新标签页创建一个独立实例（含独立的内部窗口），使同类型多标签页各自独立、互不干扰
    return new PluginMedia;
}

#include "pluginmedia.h"

#include <QFileInfo>

PluginMedia::PluginMedia(QObject *parent) : Base(parent)
{
    // 内存数据交给窗口侧解码（与原有做法一致：经由信号转到 MainWindow::showVideo）
    connect(this, &PluginMedia::showVideo, m_wMainWindow, &MainWindow::showVideo);
}

bool PluginMedia::showFile(const PluginFile &file)
{
    QByteArray owned;
    const char *pData = nullptr;
    qint64 nLen = 0;

    if (file.isPath())
    {
        owned = file.readAll();
        pData = owned.constData();
        nLen  = owned.size();
    }
    else
    {
        pData = file.memory().constData();
        nLen  = file.memory().size();
    }
    if (pData == nullptr || nLen <= 0)
        return false;

    // 直接调用窗口侧 showVideo 以拿到 int 返回值（信号是 void，失败状态会被丢弃）。
    // 失败时统一复位进度条/时间标签/播放图标，避免残留上一个文件的状态。
    const int nRet = m_wMainWindow->showVideo(const_cast<char *>(pData), nLen);
    if (nRet < 0)
    {
        m_wMainWindow->clearContent();
        return false;
    }
    return true;
}

bool PluginMedia::stopPlaybackAndWait()
{
    return m_wMainWindow ? m_wMainWindow->stopPlaybackAndWait() : true;
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

bool PluginMedia::prefersFileByPath() const
{
    return true;
}

bool PluginMedia::sendFileByPath(const QString &path, const QString &displayName)
{
    Q_UNUSED(displayName);
    if (path.isEmpty() || !QFileInfo::exists(path) || m_wMainWindow == nullptr)
        return false;
    // 临时文件在整个会话期间有效，FFmpeg 的后台解码线程可以持续按路径读取
    return m_wMainWindow->showVideoFile(path) >= 0;
}

bool PluginMedia::sendFileMapped(char *pData, qint64 nFileLen, const QString &displayName)
{
    Q_UNUSED(displayName);
    if (m_wMainWindow == nullptr || pData == nullptr || nFileLen <= 0)
        return false;
    if (m_wMainWindow->showVideoMapped(pData, nFileLen) < 0)
    {
        m_wMainWindow->clearContent();
        return false;
    }
    return true;
}

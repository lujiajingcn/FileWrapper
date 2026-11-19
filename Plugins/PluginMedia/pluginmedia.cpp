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

    // 每次尝试开始先清掉上一次的失败原因（接口 v6 约定）：宿主只在返回 false 后
    // 读 lastError()，陈旧原因会把这次的失败解释错。
    m_wMainWindow->clearLastError();

    if (file.isPath())
    {
        // P1-23：路径分支不要无上限 readAll。正常宿主会走 sendFileByPath 流式打开，
        // 这里只服务直接 API 调用；超过阈值直接拒绝，避免一次性读入数 GB。
        constexpr qint64 kMaxDirectPathBytes = 256LL * 1024 * 1024;
        if (file.size() > kMaxDirectPathBytes)
            return false;
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
        return reportOpenFailure();
    return true;
}

QString PluginMedia::lastError() const
{
    // 空串 = 本次没失败（或旧窗口没有原因）：宿主据此决定显示通用提示。
    return m_wMainWindow ? m_wMainWindow->lastError() : QString();
}

bool PluginMedia::reportOpenFailure()
{
    if (m_wMainWindow == nullptr)
        return false;
    // 顺序不能反：clearContent() 会清空画面区，真实原因必须在它之后写进去。
    // 画面区是本插件唯一的"持久"位置 —— 宿主的提示只停 10 秒，
    // 用户回头看标签页时不能只剩一块黑屏。
    m_wMainWindow->clearContent();
    m_wMainWindow->showFailurePage();
    return false;
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
    if (m_wMainWindow == nullptr)
        return false;
    m_wMainWindow->clearLastError();
    // 非法输入与 showFile/sendFileMapped 的空数据分支保持一致：只返回 false，
    // 由宿主决定是否复位（不在这里产生"停掉正在播放文件"的副作用）。
    if (path.isEmpty() || !QFileInfo::exists(path))
        return false;
    // 临时文件在整个会话期间有效，FFmpeg 的后台解码线程可以持续按路径读取
    if (m_wMainWindow->showVideoFile(path) < 0)
    {
        // 打开失败（showVideo 已停掉旧播放并清画面，但进度条/时间/图标仍是旧状态）
        return reportOpenFailure();
    }
    return true;
}

bool PluginMedia::sendFileMapped(char *pData, qint64 nFileLen, const QString &displayName)
{
    Q_UNUSED(displayName);
    if (m_wMainWindow == nullptr)
        return false;
    m_wMainWindow->clearLastError();
    if (pData == nullptr || nFileLen <= 0)
        return false;
    if (m_wMainWindow->showVideoMapped(pData, nFileLen) < 0)
        return reportOpenFailure();
    return true;
}

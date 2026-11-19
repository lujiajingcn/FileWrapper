#include "pluginpicture.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>

bool PluginPicture::showFile(const PluginFile &file)
{
    // P1-23：路径形态先用 QFile::size() 做上限检查，再 readAll —— 否则一个
    // 数 GiB 的文件会先被整个读进内存（整块读入还会再复制一份给 QByteArray）。
    constexpr qint64 kMaxPathBytes = 256LL * 1024 * 1024;   // 256 MiB
    if (file.isPath())
    {
        const QString sPath = file.path();
        // 先用 QFileInfo 拿到读取前的大小；读不到（-1）或空文件直接放弃
        const qint64 nSizeBefore = sPath.isEmpty() ? -1 : QFileInfo(sPath).size();
        if (nSizeBefore <= 0 || nSizeBefore > kMaxPathBytes)
        {
            if (nSizeBefore > kMaxPathBytes)
                qWarning() << "[PluginPicture] 文件超过 256 MiB，拒绝读入内存:" << sPath;
            m_wMainWindow->clearContent();
            return false;
        }

        QFile f(sPath);
        if (!f.open(QIODevice::ReadOnly))
        {
            m_wMainWindow->clearContent();
            return false;
        }
        const QByteArray data = f.readAll();
        // 读取期间文件被改写/截断时长度会对不上：长度必须等于读取前的大小，
        // 且 QFile::size() 读后仍是同一值，才认为拿到了完整内容。
        if (data.size() != nSizeBefore || f.size() != nSizeBefore)
        {
            qWarning() << "[PluginPicture] 读取期间文件大小发生变化，放弃显示:" << sPath;
            m_wMainWindow->clearContent();
            return false;
        }
        if (data.isEmpty())
        {
            m_wMainWindow->clearContent();
            return false;
        }
        m_wMainWindow->showPicture(const_cast<char *>(data.constData()), data.size());
        return true;
    }

    const QByteArray &data = file.memory();
    if (data.isEmpty())
        return false;
    m_wMainWindow->showPicture(const_cast<char *>(data.constData()), data.size());
    return true;
}

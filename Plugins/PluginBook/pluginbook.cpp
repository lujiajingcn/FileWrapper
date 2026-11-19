#include "pluginbook.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>

bool PluginBook::showFile(const PluginFile &file)
{
    // P1-23：路径形态先查大小再 readAll —— 与 showBook() 内部的整书 256 MiB
    // 预算保持一致，避免超大文件先被整个读进内存、再被 showBook() 拒绝。
    constexpr qint64 kMaxPathBytes = 256LL * 1024 * 1024;   // 256 MiB
    if (file.isPath())
    {
        const QString sPath = file.path();
        // 读取前的大小（读不到为 -1）：为空/不可读/超限都直接放弃
        const qint64 nSizeBefore = sPath.isEmpty() ? -1 : QFileInfo(sPath).size();
        if (nSizeBefore <= 0 || nSizeBefore > kMaxPathBytes)
        {
            if (nSizeBefore > kMaxPathBytes)
                qWarning() << "[PluginBook] 文件超过 256 MiB，拒绝读入内存:" << sPath;
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
        // 读取前后大小必须一致（等于读取前的大小且 QFile::size() 未变），
        // 否则说明文件在读取期间被截断/改写，放弃显示以免使用半截数据。
        if (data.size() != nSizeBefore || f.size() != nSizeBefore)
        {
            qWarning() << "[PluginBook] 读取期间文件大小发生变化，放弃显示:" << sPath;
            m_wMainWindow->clearContent();
            return false;
        }
        if (data.isEmpty())
        {
            m_wMainWindow->clearContent();
            return false;
        }
        m_wMainWindow->showBook(const_cast<char *>(data.constData()), data.size());
        return true;
    }

    const QByteArray &data = file.memory();
    if (data.isEmpty())
        return false;
    // showBook 内部会复制一份并持有（m_bookData），这里传只读指针安全
    m_wMainWindow->showBook(const_cast<char *>(data.constData()), data.size());
    return true;
}

void PluginBook::setSourceName(const QString &name)
{
    if (m_wMainWindow) m_wMainWindow->setSourceName(name);
}

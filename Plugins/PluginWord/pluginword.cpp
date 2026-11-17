#include "pluginword.h"

bool PluginWord::showFile(const PluginFile &file)
{
    // C2：内存通道直接把 QByteArray 交给窗口，省掉旧通道的一次完整复制。
    if (file.isMemory())
    {
        if (file.memory().isEmpty())
            return false;
        m_wMainWindow->showWord(file.memory());
        return true;
    }

    // 本插件恒返回 prefersFileByPath()=false，主程序不会递路径过来；
    // 万一将来改了，就读进内存再处理（语义与旧通道一致）。
    const QByteArray data = file.readAll();
    if (data.isEmpty())
        return false;
    m_wMainWindow->showWord(data);
    return true;
}

void PluginWord::setSourceName(const QString &name)
{
    if (m_wMainWindow) m_wMainWindow->setSourceName(name);
}

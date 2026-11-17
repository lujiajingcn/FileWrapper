#include "plugintext.h"

bool PluginText::showFile(const PluginFile &file)
{
    if (file.isPath())
    {
        // 本插件只吃字节：路径形态就读进内存（Text 不声明 prefersFileByPath）
        const QByteArray data = file.readAll();
        if (data.isEmpty())
            return false;
        m_wMainWindow->showText(const_cast<char *>(data.constData()), data.size());
        return true;
    }

    const QByteArray &data = file.memory();
    if (data.isEmpty())
        return false;
    // showText(char*, len) 内部会复制一份，这里不需要额外 Const_cast 生命周期管理
    m_wMainWindow->showText(const_cast<char *>(data.constData()), data.size());
    return true;
}

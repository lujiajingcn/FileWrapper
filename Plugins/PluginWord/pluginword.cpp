#include "pluginword.h"

PluginWord::PluginWord(QObject *parent) : QObject(parent)
{
    m_wMainWindow = new MainWindow;
}

PluginWord::~PluginWord()
{
    delete m_wMainWindow;
    m_wMainWindow = nullptr;
}

void PluginWord::sendFileData(char *szFileData, qint64 nFileLen)
{
    if (szFileData == nullptr || nFileLen <= 0)
    {
        // 主程序在读文件失败 / 0 字节时也会送 (nullptr, 0)：必须清掉内容区，
        // 否则界面仍停留在上一个文件。
        if (m_wMainWindow) m_wMainWindow->clearContent();
        return;
    }
    m_wMainWindow->showWord(szFileData, nFileLen);
}

QWidget* PluginWord::getPluginWidget()
{
    return m_wMainWindow;
}

PluginInterface* PluginWord::createInstance()
{
    // 为新标签页创建一个独立实例（含独立的内部窗口），使同类型多标签页各自独立、互不干扰
    return new PluginWord;
}

void PluginWord::setSourceName(const QString &name)
{
    if (m_wMainWindow) m_wMainWindow->setSourceName(name);
}

bool PluginWord::sendFile(const PluginFile &file)
{
    // C2：内存通道 —— 直接把 QByteArray 交给窗口，省掉旧通道的一次完整复制。
    if (file.isMemory())
    {
        if (file.memory().isEmpty())
        {
            if (m_wMainWindow) m_wMainWindow->clearContent();
            return false;
        }
        m_wMainWindow->showWord(file.memory());
        return true;
    }

    // 本插件恒返回 prefersFileByPath()=false，主程序不会递路径过来；
    // 万一将来改了，就读进内存再处理（语义与旧通道一致）。
    const QByteArray data = file.readAll();
    if (data.isEmpty())
    {
        if (m_wMainWindow) m_wMainWindow->clearContent();
        return false;
    }
    m_wMainWindow->showWord(data);
    return true;
}

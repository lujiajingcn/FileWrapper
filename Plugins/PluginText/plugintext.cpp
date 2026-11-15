#include "plugintext.h"

PluginText::PluginText(QObject *parent) : QObject(parent)
{
    m_wMainWindow = new MainWindow;
}

PluginText::~PluginText()
{
    delete m_wMainWindow;
    m_wMainWindow = nullptr;
}

void PluginText::sendFileData(char *szFileData, qint64 nFileLen)
{
    if (szFileData == nullptr || nFileLen <= 0)
    {
        // 主程序在读文件失败 / 0 字节时也会送 (nullptr, 0)：必须清掉内容区，
        // 否则界面仍停留在上一个文件。
        if (m_wMainWindow) m_wMainWindow->clearContent();
        return;
    }
    m_wMainWindow->showText(szFileData, nFileLen);
}

QWidget* PluginText::getPluginWidget()
{
    return m_wMainWindow;
}

PluginInterface* PluginText::createInstance()
{
    // 为新标签页创建一个独立实例（含独立的内部窗口），使同类型多标签页各自独立、互不干扰
    return new PluginText;
}

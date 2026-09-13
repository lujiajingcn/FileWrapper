#include "pluginbook.h"

PluginBook::PluginBook(QObject *parent) : QObject(parent)
{
    m_wMainWindow = new MainWindow;
}

PluginBook::~PluginBook()
{
    delete m_wMainWindow;
    m_wMainWindow = nullptr;
}

void PluginBook::sendFileData(char *szFileData, qint64 nFileLen)
{
    if (szFileData == nullptr || nFileLen <= 0)
        return;
    m_wMainWindow->showBook(szFileData, nFileLen);
}

QWidget* PluginBook::getPluginWidget()
{
    return m_wMainWindow;
}

PluginInterface* PluginBook::createInstance()
{
    // 为新标签页创建一个独立实例（含独立的内部窗口），使同类型多标签页各自独立、互不干扰
    return new PluginBook;
}

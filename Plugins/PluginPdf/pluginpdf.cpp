#include "pluginpdf.h"

PluginPdf::PluginPdf(QObject *parent) : QObject(parent)
{
    m_wMainWindow = new MainWindow;
}

PluginPdf::~PluginPdf()
{
    delete m_wMainWindow;
    m_wMainWindow = nullptr;
}

void PluginPdf::sendFileData(char *szFileData, qint64 nFileLen)
{
    if (szFileData == nullptr || nFileLen <= 0)
        return;
    m_wMainWindow->showPdf(szFileData, nFileLen);
}

QWidget* PluginPdf::getPluginWidget()
{
    return m_wMainWindow;
}

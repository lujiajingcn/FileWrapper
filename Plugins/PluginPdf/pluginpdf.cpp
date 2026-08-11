#include "pluginpdf.h"

PluginPdf::PluginPdf(QObject *parent) : QObject(parent)
{
    m_wMainWindow = new MainWindow;
}

void PluginPdf::sendFileData(char *szFileData, qint64 nFileLen)
{
    m_wMainWindow->showPdf(szFileData, nFileLen);
}

QWidget* PluginPdf::getPluginWidget()
{
    return m_wMainWindow;
}

#include "filemanager.h"
#include <QApplication>
#include <QDebug>
#include <cstring>

#define FILEPATHSIZE 260

#define FILELENMAX 20
#define FILECOUNTMAX 20
#define QHEADERITEMSIZE FILEPATHSIZE + FILELENMAX + FILECOUNTMAX

#define MAXBUFFERSIZE 1024 * 1024 * 4

// todo 分块读写大文件

FileManager::FileManager(QObject *parent) : QObject(parent)
{
}

//INTSIZE 【FILEPATHSIZE，INTSIZE，INTSIZE】【FILEPATHSIZE，INTSIZE，INTSIZE】...
//文件个数【文件路径，文件大小，文件内容位置】【文件路径，文件大小，文件内容位置】...【文件内容】【文件内容】...
void FileManager::QMergeFiles(QVector<QString> vtInputFiles, QString sOutputFile)
{
    // 在文件头记录合并文件的个数
    int nCount = vtInputFiles.size();
    QFile fOutputFile(sOutputFile);
    if(!fOutputFile.open(QIODevice::WriteOnly))
    {
        qWarning()<<"打开输出文件失败:"<<sOutputFile;
        return;
    }
    if(!fOutputFile.seek(0))
    {
        qWarning()<<"定位输出文件失败:"<<sOutputFile;
        fOutputFile.close();
        return;
    }
    {
        char szBuf[FILECOUNTMAX];
        memset(szBuf, 0x0, FILECOUNTMAX);
        QString sCount = QString::number(nCount);
        strncpy(szBuf, sCount.toStdString().c_str(), FILECOUNTMAX - 1);
        if(fOutputFile.write(szBuf, FILECOUNTMAX) != FILECOUNTMAX)
        {
            qWarning()<<"写入文件个数失败:"<<sOutputFile;
            fOutputFile.close();
            return;
        }
    }

    // 文件头部分的大小
    qint64 nHeaderSize = FILECOUNTMAX + (FILEPATHSIZE + FILELENMAX + FILELENMAX) * vtInputFiles.size();

    // 文件内容的存放位置
    qint64 nFileContentPosition = nHeaderSize;

    // 将要合并的文件的信息（包括文件路径，文件大小，文件内容的存放位置）写入合并后的文件
    for (QVector<QString>::const_iterator cIt = vtInputFiles.begin(); cIt != vtInputFiles.end(); cIt++)
    {
        QString strFilePath = *cIt;
        QFile fInputFile(strFilePath);
        if(!fInputFile.open(QIODevice::ReadOnly))
        {
            qWarning()<<"打开输入文件失败:"<<strFilePath;
            fOutputFile.close();
            return;
        }
        qint64 nFileLen = fInputFile.size();
        fInputFile.close();
        {
            char szPath[FILEPATHSIZE];
            memset(szPath, 0x0, FILEPATHSIZE);
            strncpy(szPath, strFilePath.toStdString().c_str(), FILEPATHSIZE - 1);
            if(fOutputFile.write(szPath, FILEPATHSIZE) != FILEPATHSIZE)
            {
                qWarning()<<"写入文件路径失败:"<<strFilePath;
                fOutputFile.close();
                return;
            }
        }
        {
            char szLen[FILELENMAX];
            memset(szLen, 0x0, FILELENMAX);
            QString sLen = QString::number(nFileLen);
            strncpy(szLen, sLen.toStdString().c_str(), FILELENMAX - 1);
            if(fOutputFile.write(szLen, FILELENMAX) != FILELENMAX)
            {
                qWarning()<<"写入文件大小失败:"<<strFilePath;
                fOutputFile.close();
                return;
            }
        }
        {
            char szPos[FILELENMAX];
            memset(szPos, 0x0, FILELENMAX);
            QString sPos = QString::number(nFileContentPosition);
            strncpy(szPos, sPos.toStdString().c_str(), FILELENMAX - 1);
            if(fOutputFile.write(szPos, FILELENMAX) != FILELENMAX)
            {
                qWarning()<<"写入文件位置失败:"<<strFilePath;
                fOutputFile.close();
                return;
            }
        }
        nFileContentPosition += nFileLen;
    }

    // 将要合并的文件的内容写入合并后的文件
    char *szBuf = new char[MAXBUFFERSIZE];
    nFileContentPosition = nHeaderSize;
    for (QVector<QString>::const_iterator cIt = vtInputFiles.begin(); cIt != vtInputFiles.end(); cIt++)
    {
        QString strFilePath = *cIt;
        QFile fInputFile(strFilePath);
        if(!fInputFile.open(QIODevice::ReadOnly))
        {
            qWarning()<<"打开输入文件失败:"<<strFilePath;
            fOutputFile.close();
            delete[] szBuf;
            return;
        }
        qint64 nFileLen = fInputFile.size();

        if(!fOutputFile.seek(nFileContentPosition))
        {
            qWarning()<<"定位输出文件位置失败:"<<strFilePath;
            fInputFile.close();
            fOutputFile.close();
            delete[] szBuf;
            return;
        }
        qint64 nBufferSize = 0;
        while(nBufferSize < nFileLen)
        {
            qint64 nBufferRealSize = fInputFile.read(szBuf, (qint64)MAXBUFFERSIZE);
            if(nBufferRealSize <= 0)
            {
                qWarning()<<"读取输入文件内容失败:"<<strFilePath;
                break;
            }
            if(fOutputFile.write(szBuf, nBufferRealSize) != nBufferRealSize)
            {
                qWarning()<<"写入输出文件内容失败:"<<sOutputFile;
                break;
            }
            nBufferSize += nBufferRealSize;
            qDebug()<<"strFilePath:"<<strFilePath<<"nBufferSize:"<<nBufferSize<<"nBufferRealSize:"<<nBufferRealSize;
        }
        fInputFile.close();

        nFileContentPosition += nFileLen;
    }
    delete[] szBuf;

    fOutputFile.close();
}

// 分割文件
// 两种方式：
// 1.根据文件中保存的文件路径存放。
// 2.根据用户选择的路径存放。
void FileManager::QSplitFiles(QString sInputFile, bool bIsSaveAsOldPath, QString sSplitFileDir)
{
    QFile fInputFile(sInputFile);
    if(!fInputFile.open(QIODevice::ReadOnly))
    {
        qWarning()<<"打开合并文件失败:"<<sInputFile;
        return;
    }
    if(!fInputFile.seek(0))
    {
        qWarning()<<"定位合并文件失败:"<<sInputFile;
        fInputFile.close();
        return;
    }

    // 获取文件数量
    char szFileCount[FILECOUNTMAX];
    memset(szFileCount, 0x0, FILECOUNTMAX);
    if(fInputFile.read(szFileCount, FILECOUNTMAX) != FILECOUNTMAX)
    {
        qWarning()<<"读取文件数量失败:"<<sInputFile;
        fInputFile.close();
        return;
    }
    int nFileCount = QString(szFileCount).toInt();

    char *szBuf = new char[MAXBUFFERSIZE];
    int nPosition = FILECOUNTMAX;
    for(int i = 0; i < nFileCount; i++)
    {
        char szFilePath[FILEPATHSIZE];
        memset(szFilePath, 0x0, FILEPATHSIZE);
        if(fInputFile.read(szFilePath, FILEPATHSIZE) != FILEPATHSIZE)
        {
            qWarning()<<"读取文件路径失败:"<<sInputFile;
            break;
        }

        QString sOutputPath;
        if(bIsSaveAsOldPath)
        {
            sOutputPath = szFilePath;
        }
        else
        {
            QString sFileName = getFileName(szFilePath);
            sOutputPath = sSplitFileDir + "/" + sFileName;
        }

        // 读取文件长度
        qint64 nFileLen = 0;
        char szFileLen[FILELENMAX];
        memset(szFileLen, 0x0, FILELENMAX);
        if(fInputFile.read(szFileLen, FILELENMAX) != FILELENMAX)
        {
            qWarning()<<"读取文件长度失败:"<<sInputFile;
            break;
        }
        nFileLen = QString(szFileLen).toLongLong();

        // 读取文件内容的存放位置
        qint64 nFilePosition = 0;
        char szFilePosition[FILELENMAX];
        memset(szFilePosition, 0x0, FILELENMAX);
        if(fInputFile.read(szFilePosition, FILELENMAX) != FILELENMAX)
        {
            qWarning()<<"读取文件位置失败:"<<sInputFile;
            break;
        }
        nFilePosition = QString(szFilePosition).toLongLong();

        // 跳转到文件内容的存放位置
        if(!fInputFile.seek(nFilePosition))
        {
            qWarning()<<"定位文件内容失败:"<<sInputFile;
            break;
        }

        // 新建一个文件
        QFile fOutputFile(sOutputPath);
        if(!fOutputFile.open(QIODevice::WriteOnly))
        {
            qWarning()<<"打开输出文件失败:"<<sOutputPath;
            // 跳转到下一个文件的头信息部分
            nPosition += QHEADERITEMSIZE;
            if(!fInputFile.seek(nPosition))
            {
                qWarning()<<"定位下一个文件头失败:"<<sInputFile;
                break;
            }
            continue;
        }

        qint64 nBufferSize = 0;
        while(nBufferSize < nFileLen)
        {
            qint64 nBufferRealSize = 0;
            if(nFileLen - nBufferSize < MAXBUFFERSIZE)
            {
                nBufferRealSize = fInputFile.read(szBuf, nFileLen - nBufferSize);
            }
            else
            {
                nBufferRealSize = fInputFile.read(szBuf, (qint64)MAXBUFFERSIZE);
            }
            if(nBufferRealSize <= 0)
            {
                qWarning()<<"读取文件内容失败:"<<sInputFile;
                break;
            }
            if(fOutputFile.write(szBuf, nBufferRealSize) != nBufferRealSize)
            {
                qWarning()<<"写入输出文件失败:"<<sOutputPath;
                break;
            }
            nBufferSize += nBufferRealSize;
            qDebug()<<"sOutputPath:"<<sOutputPath<<"nBufferSize:"<<nBufferSize<<"nBufferRealSize:"<<nBufferRealSize;
        }

        fOutputFile.close();

        // 跳转到下一个文件的头信息部分
        nPosition += QHEADERITEMSIZE;
        if(!fInputFile.seek(nPosition))
        {
            qWarning()<<"定位下一个文件头失败:"<<sInputFile;
            break;
        }
    }
    delete []szBuf;
    fInputFile.close();
}

// 加载合并后的文件，分析文件信息
void FileManager::QLoadMergedFile(QString sFilePath)
{
    m_vtFileInfos.clear();
    m_mapFileInfos.clear();

    // 关闭已打开的旧句柄，避免句柄泄漏
    if(m_qFile.isOpen())
        m_qFile.close();

    m_qFile.setFileName(sFilePath);
    if(!m_qFile.open(QIODevice::ReadOnly))
    {
        qWarning()<<"打开合并文件失败:"<<sFilePath;
        return;
    }
    if(!m_qFile.seek(0))
    {
        qWarning()<<"定位合并文件失败:"<<sFilePath;
        m_qFile.close();
        return;
    }
    char szFileCount[FILECOUNTMAX];
    memset(szFileCount, 0x0, FILECOUNTMAX);
    if(m_qFile.read(szFileCount, FILECOUNTMAX) != FILECOUNTMAX)
    {
        qWarning()<<"读取文件数量失败:"<<sFilePath;
        m_qFile.close();
        return;
    }
    int nFileCount = QString(szFileCount).toInt();

    int nPosition = FILECOUNTMAX;
    for(int i = 0; i < nFileCount; i++)
    {
        FILEINFO fileInfo;

        char szFilePath[FILEPATHSIZE];
        memset(szFilePath, 0x0, FILEPATHSIZE);
        if(m_qFile.read(szFilePath, FILEPATHSIZE) != FILEPATHSIZE)
        {
            qWarning()<<"读取文件路径失败:"<<sFilePath;
            m_vtFileInfos.clear();
            m_mapFileInfos.clear();
            m_qFile.close();
            return;
        }

        fileInfo.sFilePath = szFilePath;

        // 读取文件长度
        qint64 nFileLen = 0;
        char szFileLen[FILELENMAX];
        memset(szFileLen, 0x0, FILELENMAX);
        if(m_qFile.read(szFileLen, FILELENMAX) != FILELENMAX)
        {
            qWarning()<<"读取文件长度失败:"<<sFilePath;
            m_vtFileInfos.clear();
            m_mapFileInfos.clear();
            m_qFile.close();
            return;
        }
        nFileLen = QString(szFileLen).toLongLong();
        fileInfo.nFileLen = nFileLen;

        // 读取文件内容的存放位置
        qint64 nFilePosition = 0;
        char szFilePosition[FILELENMAX];
        memset(szFilePosition, 0x0, FILELENMAX);
        if(m_qFile.read(szFilePosition, FILELENMAX) != FILELENMAX)
        {
            qWarning()<<"读取文件位置失败:"<<sFilePath;
            m_vtFileInfos.clear();
            m_mapFileInfos.clear();
            m_qFile.close();
            return;
        }
        nFilePosition = QString(szFilePosition).toLongLong();
        fileInfo.nContentPositon = nFilePosition;

        // 分析文件名
        fileInfo.sFileName = getFileName(fileInfo.sFilePath);

        m_vtFileInfos.push_back(fileInfo);
        m_mapFileInfos[fileInfo.sFilePath] = fileInfo;

        // 跳转到下一个文件的头信息部分
        nPosition += QHEADERITEMSIZE;
        if(!m_qFile.seek(nPosition))
        {
            qWarning()<<"定位下一个文件头失败:"<<sFilePath;
            m_vtFileInfos.clear();
            m_mapFileInfos.clear();
            m_qFile.close();
            return;
        }
    }
}

void FileManager::unLoadMergedFile()
{
    if(m_qFile.isOpen())
        m_qFile.close();
    m_vtFileInfos.clear();
    m_mapFileInfos.clear();
}

// 读取文件内容。注意：*szBuf 由本方法用 new[] 分配，调用方必须负责 delete[] 释放。
void FileManager::QGetFileContent(QString sFilePath, char **szBuf, qint64 &nFileLen)
{
    *szBuf = nullptr;
    nFileLen = 0;

    QMap<QString,FILEINFO>::const_iterator cIt = m_mapFileInfos.find(sFilePath);
    if(cIt == m_mapFileInfos.end())
    {
        qDebug()<<"未找到文件:"<<sFilePath;
        return;
    }

    nFileLen = cIt.value().nFileLen;
    if(nFileLen <= 0)
        return;

    // 跳转到文件内容的存放位置
    if(!m_qFile.seek(cIt.value().nContentPositon))
    {
        qWarning()<<"定位文件内容失败:"<<sFilePath;
        nFileLen = 0;
        return;
    }

    *szBuf = new char[nFileLen];
    // 读取文件内容
    qint64 nRead = m_qFile.read(*szBuf, nFileLen);
    if(nRead != nFileLen)
    {
        qWarning()<<"读取文件内容不完整:"<<sFilePath<<"期望"<<nFileLen<<"实际"<<nRead;
        delete[] *szBuf;
        *szBuf = nullptr;
        nFileLen = 0;
        return;
    }
}

QVector<FILEINFO> FileManager::getFileInfos()
{
    return m_vtFileInfos;
}

void FileManager::outputFile(QString sFilePath, QString sOutputDir)
{
    QString sOutputFilePath;
    if(!sOutputDir.isEmpty())
    {
        sOutputFilePath = sOutputDir + "/" + getFileName(sFilePath);
    }

    char *szBuf = nullptr;
    qint64 nFileLen = 0;

    QGetFileContent(sFilePath, &szBuf, nFileLen);

    if(szBuf == nullptr)
    {
        qDebug()<<"导出失败，未获取到文件内容:"<<sFilePath;
        return;
    }

    QFile fTemp(sOutputFilePath);
    if(!fTemp.open(QIODevice::WriteOnly))
    {
        qWarning()<<"打开输出文件失败:"<<sOutputFilePath;
        delete[] szBuf;
        return;
    }
    if(fTemp.write(szBuf, nFileLen) != nFileLen)
    {
        qWarning()<<"写入输出文件失败:"<<sOutputFilePath;
    }
    fTemp.close();

    // 释放 QGetFileContent 分配的内容
    delete[] szBuf;
}

// 从文件路径获取带后缀的文件名
QString FileManager::getFileName(QString sFilePath)
{
    int nPosition = (sFilePath.lastIndexOf('\\') + 1) == 0 ?  sFilePath .lastIndexOf('/') + 1: sFilePath .lastIndexOf('\\') + 1 ;
    return sFilePath.right(sFilePath.length() - nPosition);
}

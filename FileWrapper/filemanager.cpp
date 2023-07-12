#include "filemanager.h"
#include <QApplication>

#define FILEPATHSIZE 260

// QFile::write 写入数字时，好像是，数字是多少位，就占用几个字节。
// 比如
// fOutputFile.write(1024, sizeof(int));
// char sTemp[4];
// memset(sTemp, 0x0, 4);
// fOutputFile.read(sTemp, 4);
// 读取的时候就会出错
#define FILELENMAX 20
#define FILECOUNTMAX 20
#define QHEADERITEMSIZE FILEPATHSIZE + FILELENMAX + FILECOUNTMAX

// todo 分块读写大文件

FileManager::FileManager(QObject *parent) : QObject(parent)
{
    m_pFile = nullptr;
    m_sMergedFilePath = "";
}

//INTSIZE 【FILEPATHSIZE，INTSIZE，INTSIZE】【FILEPATHSIZE，INTSIZE，INTSIZE】...
//文件个数【文件路径，文件大小，文件内容位置】【文件路径，文件大小，文件内容位置】...【文件内容】【文件内容】...
void FileManager::QMergeFiles(QVector<QString> vtInputFiles, QString sOutputFile)
{
    // 在文件头记录合并文件的个数
    int nCount = vtInputFiles.size();
    QFile fOutputFile(sOutputFile);
    fOutputFile.open(QIODevice::WriteOnly);
    fOutputFile.seek(0);
    fOutputFile.write(QString::number(nCount).toStdString().c_str(), FILECOUNTMAX);

    // 文件头部分的大小
    qint64 nHeaderSize = FILECOUNTMAX + (FILEPATHSIZE + FILELENMAX + FILELENMAX) * vtInputFiles.size();

    // 文件内容的存放位置
    int nFileContentPosition = nHeaderSize;

    // 将要合并的文件的信息（包括文件路径，文件大小，文件内容的存放位置）写入合并后的文件
    for (QVector<QString>::const_iterator cIt = vtInputFiles.begin(); cIt != vtInputFiles.end(); cIt++)
    {
        QString strFilePath = *cIt;
        QFile fInputFile(strFilePath);
        fInputFile.open(QIODevice::ReadOnly);
        int nFileLen = fInputFile.size();
        fInputFile.close();
        fOutputFile.write(strFilePath.toStdString().c_str(), FILEPATHSIZE);                         // 写入文件路径
        fOutputFile.write(QString::number(nFileLen).toStdString().c_str(), FILELENMAX);             // 写入文件大小
        fOutputFile.write(QString::number(nFileContentPosition).toStdString().c_str(), FILELENMAX); // 写入文件内容在合并后的文件中的开始位置
        nFileContentPosition += nFileLen;
    }

    // 将要合并的文件的内容写入合并后的文件
    nFileContentPosition = nHeaderSize;
    for (QVector<QString>::const_iterator cIt = vtInputFiles.begin(); cIt != vtInputFiles.end(); cIt++)
    {
        QString strFilePath = *cIt;
        QFile fInputFile(strFilePath);
        fInputFile.open(QIODevice::ReadOnly);
        qint64 nFileLen = fInputFile.size();
        char *szBuf = new char[nFileLen];
        memset(szBuf, 0x0, nFileLen);
        fInputFile.read(szBuf, nFileLen);
        fInputFile.close();

        fOutputFile.seek(nFileContentPosition);
        fOutputFile.write(szBuf, nFileLen);
        nFileContentPosition += nFileLen;
        delete[] szBuf;
    }

    fOutputFile.close();
}

// 分割文件
// 两种方式：
// 1.根据文件中保存的文件路径存放。
// 2.根据用户选择的路径存放。
void FileManager::QSplitFiles(QString sInputFile, bool bIsSaveAsOldPath, QString sSplitFileDir)
{
    QFile fInputFile(sInputFile);
    fInputFile.open(QIODevice::ReadOnly);
    fInputFile.seek(0);

    // 获取文件数量
    char *data;
    fInputFile.read(data, FILECOUNTMAX);
    int nFileCount = QString(data).toInt();

    qint64 nHeaderSize = FILECOUNTMAX + (FILEPATHSIZE + FILELENMAX + FILELENMAX) * nFileCount;

    int nPosition = FILECOUNTMAX;
    for(int i = 0; i < nFileCount; i++)
    {
        char szFilePath[FILEPATHSIZE];
        memset(szFilePath, 0x0, FILEPATHSIZE);
        fInputFile.read(szFilePath, FILEPATHSIZE);

        // 读取文件长度
        int nFileLen = 0;
        char szFileLen[FILELENMAX];
        memset(szFileLen, 0x0, FILELENMAX);
        fInputFile.read(szFileLen, FILELENMAX);
        nFileLen = QString(szFileLen).toInt();

        // 读取文件内容的存放位置
        int nFilePosition = 0;
        char szFilePosition[FILELENMAX];
        memset(szFilePosition, 0x0, FILELENMAX);
        fInputFile.read(szFilePosition, FILELENMAX);
        nFilePosition = QString(szFilePosition).toInt();

        // 跳转到文件内容的存放位置
        fInputFile.seek(nFilePosition);
        char *szBuf = new char[nFileLen];
        // 读取文件内容
        fInputFile.read(szBuf, nFileLen);

        QString sName;
        if(bIsSaveAsOldPath)
        {
            sName = szFilePath;
        }
        else
        {
            //todo
        }
        // 新建一个文件
        QFile fTemp(sName);
        fTemp.open(QIODevice::WriteOnly);
        fTemp.write(szBuf, nFileLen);
        fTemp.close();

        // 跳转到下一个文件的头信息部分
        nPosition += QHEADERITEMSIZE;
        fInputFile.seek(nPosition);
    }
}

// 加载合并后的文件，分析文件信息
void FileManager::QLoadMergedFile(QString sFilePath)
{
    m_vtFileInfos.clear();

    m_qFile.setFileName(sFilePath);
    m_qFile.open(QIODevice::ReadOnly);
    m_qFile.seek(0);
    char *data;
    m_qFile.read(data, FILECOUNTMAX); // todo 该语句会导致sFilePath变为空
    int nFileCount = QString(data).toInt();

    int nPosition = FILECOUNTMAX;
    for(int i = 0; i < nFileCount; i++)
    {
        FILEINFO fileInfo;

        char szFilePath[FILEPATHSIZE];
        memset(szFilePath, 0x0, FILEPATHSIZE);
        m_qFile.read(szFilePath, FILEPATHSIZE);

        fileInfo.sFilePath = szFilePath;

        // 读取文件长度
        int nFileLen = 0;
        char szFileLen[FILELENMAX];
        memset(szFileLen, 0x0, FILELENMAX);
        m_qFile.read(szFileLen, FILELENMAX);
        nFileLen = QString(szFileLen).toInt();
        fileInfo.nFileLen = nFileLen;

        // 读取文件内容的存放位置
        int nFilePosition = 0;
        char szFilePosition[FILELENMAX];
        memset(szFilePosition, 0x0, FILELENMAX);
        m_qFile.read(szFilePosition, FILELENMAX);
        nFilePosition = QString(szFilePosition).toInt();
        fileInfo.nContentPositon = nFilePosition;

        // 分析文件名
        fileInfo.sFileName = getFileName(fileInfo.sFilePath);

        m_vtFileInfos.push_back(fileInfo);
        m_mapFileInfos[fileInfo.sFilePath] = fileInfo;

        // 跳转到下一个文件的头信息部分
        nPosition += QHEADERITEMSIZE;
        m_qFile.seek(nPosition);
    }
}

void FileManager::unLoadMergedFile()
{
    if(m_pFile != nullptr)
        fclose(m_pFile);
}

void FileManager::QGetFileContent(QString sFilePath, char **szBuf, int &nFileLen)
{
    QMap<QString,FILEINFO>::const_iterator cIt = m_mapFileInfos.find(sFilePath);
    if(cIt == m_mapFileInfos.end())
    {
        return;
    }
    nFileLen = cIt.value().nFileLen;

    // 跳转到文件内容的存放位置
    m_qFile.seek(cIt.value().nContentPositon);

    *szBuf = new char[nFileLen];
    // 读取文件内容
    m_qFile.read(*szBuf, nFileLen);
}

QVector<FILEINFO> FileManager::getFileInfos()
{
    return m_vtFileInfos;
}

QString FileManager::getMergedFilePath()
{
    return m_sMergedFilePath;
}

void FileManager::outputFile(QString sFilePath, QString sOutputDir)
{
    QString sOutputFilePath;
    if(!sOutputDir.isEmpty())
    {
        sOutputFilePath = sOutputDir + "/" + getFileName(sFilePath);
    }

    char *szBuf = nullptr;
    int nFileLen = 0;

    QGetFileContent(sFilePath, &szBuf, nFileLen);

    QFile fTemp(sOutputFilePath);
    fTemp.open(QIODevice::WriteOnly);
    fTemp.write(szBuf, nFileLen);
    fTemp.close();
}

// 从文件路径获取带后缀的文件名
QString FileManager::getFileName(QString sFilePath)
{
    int nPosition = (sFilePath.lastIndexOf('\\') + 1) == 0 ?  sFilePath .lastIndexOf('/') + 1: sFilePath .lastIndexOf('\\') + 1 ;
    return sFilePath.right(sFilePath.length() - nPosition);
}

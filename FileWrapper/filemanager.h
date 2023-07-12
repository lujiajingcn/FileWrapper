#ifndef FILEMANAGER_H
#define FILEMANAGER_H

#include <QMap>
#include <QFile>
#include <QObject>
#include <string>
#include <iostream>
#include <io.h>
#include <fcntl.h>
#include <stdio.h>
using namespace std;

typedef struct FILEINFO
{
    QString sFilePath;          // 文件路径
    QString sFileName;          // 文件名称（包括后缀）
    int     nFileLen;           // 文件大小
    int     nContentPositon;    // 文件内容在合并后的文件中的位置
    int     nState;             // 是否已经删除
} FILEINFO;

Q_DECLARE_METATYPE(FILEINFO)

class FileManager : public QObject
{
    Q_OBJECT

public:
    explicit FileManager(QObject *parent = nullptr);

    void QLoadMergedFile(QString sFilePath);
    void unLoadMergedFile();
    void QGetFileContent(QString sFilePath, char **szBuf, int &nFileLen);
    QVector<FILEINFO> getFileInfos();
    QString getMergedFilePath();

    void outputFile(QString sFilePath, QString sOutputDir);

public slots:
    void QMergeFiles(QVector<QString> vtInputFiles, QString sOutputFile);
    void QSplitFiles(QString sInputFile, bool bIsSaveAsOldPath, QString sSplitFileDir);

private:
    QString getFileName(QString sFilePath);

private:
    QVector<FILEINFO>        m_vtFileInfos;
    QString                 m_sMergedFilePath;
    QMap<QString,FILEINFO>  m_mapFileInfos;
    FILE                    *m_pFile;
    QFile                   m_qFile;
};

#endif // FILEMANAGER_H

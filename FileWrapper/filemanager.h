#ifndef FILEMANAGER_H
#define FILEMANAGER_H

#include <QMap>
#include <QFile>
#include <QObject>

typedef struct FILEINFO
{
    QString sFilePath;          // 文件路径
    QString sFileName;          // 文件名称（包括后缀）
    qint64  nFileLen;           // 文件大小
    qint64  nContentPositon;    // 文件内容在合并后的文件中的位置
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
    void QGetFileContent(QString sFilePath, char **szBuf, qint64 &nFileLen);
    QVector<FILEINFO> getFileInfos();

    void outputFile(QString sFilePath, QString sOutputDir);
    bool validateMergedFile(QString sFilePath);

public slots:
    void QMergeFiles(QVector<QString> vtInputFiles, QString sOutputFile);
    void QSplitFiles(QString sInputFile, bool bIsSaveAsOldPath, QString sSplitFileDir);

private:
    QString getFileName(QString sFilePath);

private:
    QVector<FILEINFO>        m_vtFileInfos;
    QMap<QString,FILEINFO>  m_mapFileInfos;
    QFile                   m_qFile;
};

#endif // FILEMANAGER_H

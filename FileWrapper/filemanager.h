#ifndef FILEMANAGER_H
#define FILEMANAGER_H

#include <QMap>
#include <QSet>
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

    // 是否已加载一个有效的归档文件
    bool isArchiveLoaded() const;

    // 向当前已加载的归档追加若干文件（仅修改内存，标记为待保存）
    void addFiles(const QVector<QString> &vtNewFiles);

    // 从当前已加载的归档中删除指定路径的文件（仅修改内存，标记为待保存）
    bool deleteFile(const QString &sFilePath);

    // 将内存中的增删改动写入磁盘归档（全部删除则删除归档文件）
    bool save();

    // 是否存在尚未落盘的改动
    bool isDirty() const;

public slots:
    void QMergeFiles(QVector<QString> vtInputFiles, QString sOutputFile);
    void QSplitFiles(QString sInputFile, bool bIsSaveAsOldPath, QString sSplitFileDir);

private:
    QString getFileName(QString sFilePath);

    // 基于当前 m_vtFileInfos 重写出整个归档文件（流式边读源边写临时文件，再原子替换）
    bool rewriteArchive();

private:
    QVector<FILEINFO>        m_vtFileInfos;
    QMap<QString,FILEINFO>  m_mapFileInfos;
    QFile                   m_qFile;
    QSet<QString>           m_setPendingFiles;  // 尚未写入归档、内容需从磁盘原文件读取的路径
    bool                    m_bDirty = false;   // 是否存在尚未落盘的改动
};

#endif // FILEMANAGER_H

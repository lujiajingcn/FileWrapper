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

    // 是否已有取消请求（UI 用于区分“用户取消”与“操作失败”）
    bool isCancelRequested() const;

signals:
    // 进度通知：已处理字节数 / 总字节数 / 状态文本
    void progressChanged(qint64 nCurrent, qint64 nTotal, const QString &sStatus);
    // 操作结束通知：是否成功 / 结果或错误信息
    void operationFinished(bool bSuccess, const QString &sMsg);

public slots:
    void QMergeFiles(QVector<QString> vtInputFiles, QString sOutputFile);
    void QSplitFiles(QString sInputFile, bool bIsSaveAsOldPath, QString sSplitFileDir);
    // 请求取消正在进行的合并/保存操作（由进度对话框的“取消”按钮触发）
    void cancelOperation();

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
    bool                    m_bCancelRequested = false;  // 用户是否已请求取消当前操作
};

#endif // FILEMANAGER_H

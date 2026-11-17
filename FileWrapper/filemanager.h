#ifndef FILEMANAGER_H
#define FILEMANAGER_H

#include <QMap>
#include <QSet>
#include <QHash>
#include <QFile>
#include <QObject>
#include "archiveformat.h"

typedef struct FILEINFO
{
    QString sFilePath;          // 展示路径（v2 = 原始根 + 相对路径；v1 = 归档内原路径）
    QString sFileName;          // 文件名称（包括后缀）
    qint64  nFileLen;           // 文件大小
    qint64  nContentPositon;    // 文件内容在归档中的位置
    int     nState;             // 是否已经删除
} FILEINFO;

Q_DECLARE_METATYPE(FILEINFO)

class FileManager : public QObject
{
    Q_OBJECT

public:
    explicit FileManager(QObject *parent = nullptr);

    // 先解析到临时结构，全部校验成功后才替换内存状态与句柄；
    // 失败时保留原有归档、视图数据与脏标记不变（P1-6）
    void QLoadMergedFile(QString sFilePath);
    void unLoadMergedFile();
    void QGetFileContent(QString sFilePath, char **szBuf, qint64 &nFileLen);

    // C2：直接读出条目内容（不经过 new char[] + delete[] 的中转）。
    // 校验语义与 QGetFileContent 一致（大小、完整性、v2 内容 CRC）；失败时 psErr 给原因。
    QByteArray entryContent(const QString &sFilePath, QString *psErr = nullptr);

    // 只读取条目开头至多 nMaxBytes 字节（格式嗅探用；不会为了"看文件头"把整块载入内存）
    QByteArray entryHead(const QString &sFilePath, qint64 nMaxBytes, QString *psErr = nullptr);
    QVector<FILEINFO> getFileInfos();

    void outputFile(QString sFilePath, QString sOutputDir);

    // 完整解析 + 合法性校验（不只是头部 CRC），供加载 / 分割 / 校验共用（P2-3）
    bool validateMergedFile(QString sFilePath);
    bool validateMergedFile(QString sFilePath, QString *psErrMsg);

    bool isArchiveLoaded() const;

    void addFiles(const QVector<QString> &vtNewFiles);
    bool deleteFile(const QString &sFilePath);
    bool save();
    bool isDirty() const;
    bool isCancelRequested() const;

    // 最近一次失败的具体原因（含路径与系统错误），UI 据此给出可读提示（P2-5 / P1-3）
    QString lastError() const;
    bool isBusy() const;

    // 校验当前归档的内容区 CRC（v1 无校验字段，会明确报告「无法校验」）
    bool verifyArchive(QString *psReport);
    FwArc::Meta archiveMeta() const;

    // 读取任意归档的元数据（不做状态变更），供分割「按原路径」二次确认展示
    static bool inspectArchive(const QString &sPath, FwArc::Meta &meta, QString &sErr);

    // 磁盘空间预检（B4）：判断 dirPath 所在磁盘能否放下 needBytes（调用方自行加预留余量）。
    // 拿不到磁盘信息时返回 true（不拦；真写不下还有写入错误兜底）。
    // pAvailable 输出可用字节数（拿不到时为 -1）。可单测：传入极大 needBytes 必为 false。
    static bool hasFreeSpaceFor(const QString &dirPath, qint64 needBytes,
                                qint64 *pAvailable = nullptr);

    // 大文件流式抽取到临时文件（P1-2）：不一次性占用全量内存
    bool extractEntryToTempFile(const QString &sDisplayName, const QString &sOutPath,
                                QString &sErr);

signals:
    void progressChanged(qint64 nCurrent, qint64 nTotal, const QString &sStatus);
    void operationFinished(bool bSuccess, const QString &sMsg);
    void warningOccurred(const QString &sMsg);

public slots:
    void QMergeFiles(QVector<QString> vtInputFiles, QString sOutputFile);
    void QSplitFiles(QString sInputFile, bool bIsSaveAsOldPath, QString sSplitFileDir);
    void cancelOperation();

private:
    QString getFileName(QString sFilePath);
    bool rewriteArchive();
    // v2 快速删除：只把被删条目的 PathLength 置为 0（墓碑）并重算头部 CRC，
    // 不触碰内容区，避免为一个小删除重写整个 10 GB 归档（P1-3）
    bool markDeletedOnly(QString &sErr);

    static bool parseFromDevice(QFile &f, FwArc::Meta &meta, QString &sErr);
    bool parseFile(const QString &sPath, FwArc::Meta &meta, QString &sErr) const;
    void setError(const QString &sMsg);

    // 写 v2 头部（CRC 字段先占位），返回每条 CRC 字段与内容区的文件偏移
    bool writeV2Header(QFile &fOut, const QVector<FILEINFO> &infos,
                       const QString &sRoot, QVector<qint64> &outCrcFieldPos,
                       QVector<qint64> &outContentPos, qint64 &outHeaderSize,
                       QString &sErr);
    // 回填内容 CRC + 头部 CRC
    bool finalizeV2Header(QFile &fOut, qint64 headerSize,
                          const QVector<qint64> &crcFieldPos,
                          const QVector<quint32> &contentCrcs,
                          QString &sErr);

    bool streamCopy(QFile &fSrc, qint64 srcPos, qint64 len, QFile &fDst,
                    FwArc::Crc32 &crc, qint64 &copiedTotal, qint64 totalBytes,
                    const QString &sStatusPrefix, QString &sErr);

    // 安全选择输出路径：目录层级还原 + 冲突自动重命名，绝不静默覆盖
    QString chooseOutputPath(const QString &sBaseDir, const FwArc::Entry &e,
                             bool bUseOldPath, QSet<QString> &setUsed, int &nRenamed);

private:
    struct BusyGuard {
        explicit BusyGuard(FileManager *fm) : p(fm) { p->m_bBusy = true; p->m_bCancelRequested = false; }
        ~BusyGuard() { p->m_bBusy = false; }
        FileManager *p;
    };

    QVector<FILEINFO>        m_vtFileInfos;
    QMap<QString,FILEINFO>   m_mapFileInfos;
    QFile                    m_qFile;
    QSet<QString>            m_setPendingFiles;
    bool                     m_bDirty = false;
    bool                     m_bCancelRequested = false;
    bool                     m_bBusy = false;
    QString                  m_sLastError;
    FwArc::Meta              m_meta;
    QHash<QString,quint32>   m_mapContentCrc;
};

#endif // FILEMANAGER_H

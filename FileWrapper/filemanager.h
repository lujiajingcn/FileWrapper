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
    QString sFilePath;          // 展示路径（v2/v3 = 原始根 + 相对路径；v1 = 归档内原路径）
    QString sFileName;          // 文件名称（包括后缀）
    qint64  nFileLen;           // 文件大小
    qint64  nContentPositon;    // 文件内容在归档中的位置
    int     nState;             // 是否已经删除
    // v3 文件系统元数据（v1/v2 条目 bHasFsMetadata=false，时间/属性为 0）
    qint64  nCreatedTime = 0;   // UTC 毫秒时间戳，0 = 未知
    qint64  nModifiedTime = 0;
    qint64  nAccessedTime = 0;
    quint32 nAttributes = 0;    // FILE_ATTRIBUTE_* 的 R/H/S/A 位（见 filemetadata.h）
    bool    bHasFsMetadata = false;
} FILEINFO;

Q_DECLARE_METATYPE(FILEINFO)

class FileManager : public QObject
{
    Q_OBJECT

public:
    explicit FileManager(QObject *parent = nullptr);

    // 先解析到临时结构，全部校验成功后才替换内存状态与句柄；
    // 失败时保留原有归档、视图数据与脏标记不变（P1-6）
    // pPreParsed 非空时直接使用调用方已经完成的解析结果，避免「先 validate 再 load」
    // 对头部与条目区重复解析两遍（P1-9）。调用方必须保证该结果来自同一文件。
    bool QLoadMergedFile(QString sFilePath, const FwArc::Meta *pPreParsed = nullptr);
    void unLoadMergedFile();
    void QGetFileContent(QString sFilePath, char **szBuf, qint64 &nFileLen);

    // C2：直接读出条目内容（不经过 new char[] + delete[] 的中转）。
    // 校验语义与 QGetFileContent 一致（大小、完整性、v2 内容 CRC）；失败时 psErr 给原因。
    QByteArray entryContent(const QString &sFilePath, QString *psErr = nullptr);

    // 只读取条目开头至多 nMaxBytes 字节（格式嗅探用；不会为了"看文件头"把整块载入内存）
    QByteArray entryHead(const QString &sFilePath, qint64 nMaxBytes, QString *psErr = nullptr);
    // 校验已映射归档内容区的 CRC（无记录 CRC 的 v1 返回 true）
    bool verifyMappedCrc(const QString &sFilePath, const char *pData, qint64 nLen) const;
    // 取加载时记录的内容 CRC；v1/无记录返回 false
    bool contentCrcFor(const QString &sFilePath, quint32 *pCrc) const;
    QVector<FILEINFO> getFileInfos();

    void outputFile(QString sFilePath, QString sOutputDir);
    QString lastOutputPath() const;

    // 完整解析 + 合法性校验（不只是头部 CRC），供加载 / 分割 / 校验共用（P2-3）
    bool validateMergedFile(QString sFilePath);
    bool validateMergedFile(QString sFilePath, QString *psErrMsg);
    // P1-9：一次解析同时返回 meta，加载时不再重新解析头部与条目区
    bool validateMergedFile(QString sFilePath, FwArc::Meta *pMeta, QString *psErrMsg = nullptr);

    bool isArchiveLoaded() const;

    // 当前已加载归档的完整路径（未加载时为空），供分割等对话框预填
    QString archivePath() const;

    // 该条目是否来自"新增未保存"的文件（没有归档内偏移，不能按内容区映射）
    bool isPendingEntry(const QString &sFilePath) const
    {
        return m_setPendingFiles.contains(sFilePath);
    }

    void addFiles(const QVector<QString> &vtNewFiles);
    bool deleteFile(const QString &sFilePath);
    bool save();
    bool isDirty() const;
    // v1 归档即使用户没改动也需要重写为 v2；保存按钮据此启用
    bool needsSave() const;
    bool isCancelRequested() const;

    // 最近一次失败的具体原因（含路径与系统错误），UI 据此给出可读提示（P2-5 / P1-3）
    QString lastError() const;
    bool isBusy() const;

    // 校验当前归档的内容区 CRC（v1 无校验字段，会明确报告「无法校验」）
    bool verifyArchive(QString *psReport);
    FwArc::Meta archiveMeta() const;
    // 只取归档格式版本（v1/v2）：不用为了读一个字段把整个 Meta（含条目表）复制一份
    quint32 archiveVersion() const { return m_meta.version; }

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
    // 复位取消标志：新操作开始时必须调用，否则上一次"取消"会让后续操作立刻失败
    void resetCancel() { m_bCancelRequested = false; }

private:
    QString getFileName(QString sFilePath);
    bool rewriteArchive();
    // 计算写归档用的 root 与每个条目的存储路径（与合并逻辑保持一致）：
    // - 所有条目在同一公共根下：root = 公共根，条目存相对路径（保留目录结构）
    // - 跨盘 / 无公共根：root 为空，盘符当首段（"E:/x" -> "E/x"）
    // 同时拒绝会撞成同一路径的条目（否则保存后无法区分）。
    static bool computeStoredPaths(const QVector<FILEINFO> &infos,
                                   QString &sRoot, QStringList &storedPaths,
                                   QString &sErr);
    // P1-3：v2 墓碑式「原地改头部」快速删除已停用。
    // 它没有崩溃恢复日志：进程在 half-updated 头部时被终止，整个归档会无法加载。
    // 删除现在统一走 rewriteArchive() 的完整事务重写（临时文件 + 校验 + 替换 +
    // 重新解析 + 备份回滚），代价是大归档删除需要重写内容区。
    // 下面这个实现仅保留供后续「写前日志 + 恢复」方案参考，禁止从保存路径调用。
    bool markDeletedOnly(QString &sErr);


    static bool parseFromDevice(QFile &f, FwArc::Meta &meta, QString &sErr);
    bool parseFile(const QString &sPath, FwArc::Meta &meta, QString &sErr) const;
    void setError(const QString &sMsg);

    // P1-1：每个读操作使用自己的 QFile（不再共享 m_qFile 的可变游标）。
    // 同时完成 P1-6 的「新增文件加入后是否被外部改动」检查。
    bool openEntrySource(const QString &sFilePath, const FILEINFO &fi,
                         QFile &localFile, qint64 &srcPos, QString &sErr) const;

    // 写 v3 头部（内容 CRC 字段先占位），返回每条 CRC 字段与内容区的文件偏移。
    // pStoredPaths 非空时直接用它作为每条目的存储路径（合并时展开已有归档用）。
    bool writeArchiveHeader(QFile &fOut, const QVector<FILEINFO> &infos,
                            const QString &sRoot, QVector<qint64> &outCrcFieldPos,
                            QVector<qint64> &outContentPos, qint64 &outHeaderSize,
                            QString &sErr, const QStringList *pStoredPaths = nullptr);
    // 回填内容 CRC + 头部 CRC
    bool finalizeArchiveHeader(QFile &fOut, qint64 headerSize,
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
        explicit BusyGuard(FileManager *fm) : p(fm) { ++p->m_nBusyDepth; p->m_bCancelRequested = false; }
        ~BusyGuard() { if (p->m_nBusyDepth > 0) --p->m_nBusyDepth; }
        FileManager *p;
    };

    QVector<FILEINFO>        m_vtFileInfos;
    QMap<QString,FILEINFO>   m_mapFileInfos;
    QFile                    m_qFile;
    QSet<QString>            m_setPendingFiles;
    bool                     m_bDirty = false;
    bool                     m_bCancelRequested = false;
    int                      m_nBusyDepth = 0;
    QString                  m_sLastError;
    QString                  m_sLastOutputPath;
    FwArc::Meta              m_meta;
    QHash<QString,quint32>   m_mapContentCrc;
};

#endif // FILEMANAGER_H

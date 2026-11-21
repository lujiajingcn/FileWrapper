#ifndef FILEMANAGER_H
#define FILEMANAGER_H

#include <QMap>
#include <QSet>
#include <QHash>
#include <QFile>
#include <QObject>
#include "archiveformat.h"
#include "crypto/archivecrypto.h"

typedef struct FILEINFO
{
    QString sFilePath;          // 展示路径（原始根 + 归档内相对路径）
    QString sFileName;          // 文件名称（包括后缀）
    qint64  nFileLen;           // 文件大小
    qint64  nContentPositon;    // 文件内容在归档中的位置
    int     nState;             // 是否已经删除
    // 文件系统元数据（源文件没有可用信息时 bHasFsMetadata=false、时间/属性为 0）
    qint64  nCreatedTime = 0;   // UTC 毫秒时间戳，0 = 未知
    qint64  nModifiedTime = 0;
    qint64  nAccessedTime = 0;
    quint32 nAttributes = 0;    // FILE_ATTRIBUTE_* 的 R/H/S/A 位（见 filemetadata.h）
    bool    bHasFsMetadata = false;
    // 条目级标记（归一化后保存）
    QStringList listTags;
    // v5 加密参数（解析后填充；写入端负责生成/沿用）
    quint64    nEntryId    = 0;
    QByteArray fileNonce;       // 8 字节
    quint32    nChunkCount = 0;
    quint32    nContentCrc = 0;
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

    // C2：直接读出条目内容（不经过 new char[] + delete[] 的中转）。
    // 校验语义：大小、完整性与内容 CRC；失败时 psErr 给原因。
    QByteArray entryContent(const QString &sFilePath, QString *psErr = nullptr);

    // 只读取条目开头至多 nMaxBytes 字节（格式嗅探用；不会为了"看文件头"把整块载入内存）
    QByteArray entryHead(const QString &sFilePath, qint64 nMaxBytes, QString *psErr = nullptr);
    // 读取条目的一段明文：新增文件直接读源文件，归档条目按块解密。
    QByteArray readEntryPlain(const FILEINFO &fi, qint64 nOffset, qint64 nLen,
                              QString *psErr) const;
    // 取加载时记录的内容 CRC；条目不在归档内时返回 false
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

    // —— 快速保存（只重写归档头部）——
    //
    // 删除条目、修改标记这类改动的特点：**其它条目的内容偏移不变**。因此不必整包
    // 重写内容区，只要把新头部写回原位置即可（HeaderSize 保持不变，条目区尾部填零，
    // 内容区留下“空洞”）。v4 解析器只按 ContentPos 取内容、不要求内容区首尾相接，
    // 所以这种归档仍然合法（见 archiveformat.h 的布局说明与 parseFromDevice）。
    //
    // 判定条件：没有新增条目、剩余条目全部来自加载时的快照、新头部能放进原
    // HeaderSize 里。返回 false 时 psWhy 给出原因（UI 据此跳过停播/缩略图等待）。
    bool canSaveHeaderOnly(QString *psWhy = nullptr) const;
    // 追加保存：本次改动**包含新增文件**，但现有条目一个都不用动 —— 新内容直接
    // 追加到原内容区末尾，头部在预留区（整包写入时留的余量）内原地重写。
    // 同样不替换文件，所以不需要停播/等后台句柄；头部放不下时回落整包重写。
    bool canSaveByAppend(QString *psWhy = nullptr) const;
    // 归档内可回收的空洞字节数（0 = 已紧凑）；UI 用来显示「整理归档」的收益
    qint64 reclaimableBytes() const;
    // 整理归档：整包重写（走与保存相同的临时文件 + 校验 + 替换 + 重新解析事务）
    bool compactArchive();
    // 归档有未保存改动时，保存按钮与标题星号据此启用
    bool needsSave() const;
    bool isCancelRequested() const;

    // v4 条目标记：写入前统一归一化（去重/trim/长度校验）。
    // 失败时 psErr 给具体原因，条目保持原样；成功设置脏标记（内容相同则不置脏）。
    bool setEntryTags(const QString &sFilePath, const QStringList &listTags,
                      QString *psErr = nullptr);
    // 当前归档里出现过的全部标记（去重、按本地化顺序排序），供标记编辑器做候选。
    QStringList allTags() const;

    // 最近一次失败的具体原因（含路径与系统错误），UI 据此给出可读提示（P2-5 / P1-3）
    QString lastError() const;
    bool isBusy() const;

    // 校验当前归档的内容区 CRC（逐条目）
    bool verifyArchive(QString *psReport);
    FwArc::Meta archiveMeta() const;
    // 只取归档格式版本：不用为了读一个字段把整个 Meta（含条目表）复制一份
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

    // —— 快速保存的实现 ——
    // 真正执行“只改头部”的保存：写回滚日志 -> 原地写头部 -> 用真实解析器验收 ->
    // 失败则用日志回滚。成功后重新解析归档（刷新偏移/CRC 表）。
    bool saveHeaderOnly();
    // 真正执行“追加保存”：日志 -> 追加内容（算 CRC）-> 落盘 -> 原地重写头部 ->
    // 验收 -> 失败则回滚头部并把文件截回原长度。
    bool saveAppendOnly();

    // 头部条目的通用描述：序列化只依赖它，删除保存与追加保存共用同一套写法。
    struct HeaderSpec
    {
        QByteArray rel;          // 归档内存储路径（UTF-8）
        QStringList tags;        // 已归一化
        quint64 size = 0;        // 内容长度
        quint64 pos  = 0;        // 内容偏移
        quint32 crc  = 0;        // 内容 CRC32
        quint64 created = 0, modified = 0, accessed = 0;
        quint32 attrs = 0;
    };
    // 按 HeaderSpec 生成整段头部（长度 = nHeaderSize，尾部填零并计算头部 CRC）
    bool buildHeaderBuffer(const QVector<HeaderSpec> &specs, const QByteArray &root,
                           qint64 nHeaderSize, QByteArray &out, QString &sErr) const;
    // 按原 HeaderSize 生成新头部缓冲（删除/改标记用；其余条目沿用原 ContentPos/CRC）
    bool buildHeaderOnlyBuffer(const QVector<FILEINFO> &infos, QByteArray &out,
                               QString &sErr) const;
    // 追加保存用的条目描述：现有条目沿用快照里的位置/CRC，新条目位置从
    // pNewPositions 取（为空时按 0 占位，只用于算头部大小）。同时算出头部精确
    // 大小与需要追加的字节数。
    bool buildAppendSpecs(const QVector<FILEINFO> &infos, const QStringList &storedPaths,
                          const QHash<QString, qint64> *pNewPositions,
                          QVector<HeaderSpec> &out, qint64 &outExactHeader,
                          qint64 &outAppendBytes, QString &sErr) const;
    // 回滚日志（写前日志）路径与读写：<归档>.hdrjournal
    static QString headerJournalPath(const QString &sArchivePath);
    static bool writeHeaderJournal(const QString &sArchivePath,
                                   const QByteArray &oldHeader, QString &sErr);
    // 加载/解析前调用：日志存在且头部校验失败、写入者已退出 -> 用日志回滚到旧头部；
    // 头部有效 -> 清掉崩溃残留的日志。返回 false 表示“另一个实例正在写，稍后重试”。
    static bool recoverHeaderJournal(const QString &sArchivePath,
                                     QString *psErr = nullptr);
    // 计算写归档用的 root 与每个条目的存储路径（与合并逻辑保持一致）：
    // - 所有条目在同一公共根下：root = 公共根，条目存相对路径（保留目录结构）
    // - 跨盘 / 无公共根：root 为空，盘符当首段（"E:/x" -> "E/x"）
    // 同时拒绝会撞成同一路径的条目（否则保存后无法区分）。
    static bool computeStoredPaths(const QVector<FILEINFO> &infos,
                                   QString &sRoot, QStringList &storedPaths,
                                   QString &sErr);
    // 删除与新增都走 rewriteArchive() 的完整事务重写：
    // 临时文件 + 校验 + 替换 + 重新解析 + 备份回滚。


    static bool parseFromDevice(QFile &f, FwArc::Meta &meta, QString &sErr);
    bool parseFile(const QString &sPath, FwArc::Meta &meta, QString &sErr) const;
    void setError(const QString &sMsg);

    // P1-1：每个读操作使用自己的 QFile（不再共享 m_qFile 的可变游标）。
    // 同时完成 P1-6 的「新增文件加入后是否被外部改动」检查。
    bool openEntrySource(const QString &sFilePath, const FILEINFO &fi,
                         QFile &localFile, qint64 &srcPos, QString &sErr) const;

    // 写 v4 头部（内容 CRC 字段先占位），返回每条 CRC 字段与内容区的文件偏移。
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
                    const QString &sStatusPrefix, QString &sErr,
                    const FwArcV5::EntryCrypto *pIn = nullptr,
                    const FwArcV5::EntryCrypto *pOut = nullptr);

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
    QHash<QString,quint32>   m_mapContentCrc;    // 归档里可回收的空洞字节数（加载时算一次）：UI 每次刷新动作状态都要读，
    // 不能在 syncActionStates() 里对 m_meta.entries 做 O(n) 遍历。
    qint64                   m_nReclaimable = 0;
};

#endif // FILEMANAGER_H

#ifndef DLGFILEPROPERTY_H
#define DLGFILEPROPERTY_H

#include <QDialog>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

// 文件列表「右键 → 属性」对话框。
//
// 文件行与目录行共用这一个对话框，数据由 MainWindow 从 FileManager 和文件列表
// 模型收集（本类不直接访问归档，便于在无归档的单元测试里构造 Data 验证排版内容）：
//   文件：类型 / 位置 / 大小 / 压缩方式 / 标记
//         所属归档 / 归档格式 / 归档内路径 / 内容校验 / 状态
//   文件夹：类型 / 位置 / 大小（合计）/ 包含
//         所属归档 / 归档格式 / 归档内路径
//
// 两个约定：
//  * 行内容统一由 buildSections() 生成，界面与「复制信息」共用同一份数据，
//    不会出现「界面上是一套、复制出来是另一套」；
//  * 文件夹的大小与数量统计的是**当前列表里可见的子树**；筛选状态下只统计匹配项，
//    因此 Data::bFiltered 为真时界面与复制文本都会明确写出统计范围。
//  * FWDA 格式不保存条目的修改时间，这里就不显示时间字段（不编造一个空值）。
class DlgFileProperty : public QDialog
{
    Q_OBJECT

public:
    struct Data
    {
        QString sName;                 // 文件名 / 文件夹名（树形单链压缩后可能含 '/'）
        QString sLocation;             // 所在目录（不含末尾分隔符；空表示没有上级）
        QString sType;                 // "DOCX 文件" / "文件" / "文件夹"
        qint64  nSize = 0;             // 文件字节数；文件夹为列表显示的子树合计
        bool    bDir = false;
        int     nFileCount = 0;        // 文件夹：包含的文件数（递归）
        int     nDirCount = 0;         // 文件夹：包含的子文件夹数（递归）
        bool    bFiltered = false;     // 大小/数量是否来自当前搜索筛选结果

        QStringList listTags;          // v4 条目标记（v1–v3 归档为空）
        QString sArchivePath;          // 所属归档的完整路径
        quint32 nArchiveVersion = 0;   // 1 / 2 / 3 / 4
        QString sEntryPath;            // 归档内路径（FILEINFO::sFilePath）
        bool    bPending = false;      // 新增条目，尚未保存进归档
        bool    bHasCrc = false;
        quint32 nCrc = 0;

        // v3 文件系统元数据（v1/v2 或来源归档没有记录时 bHasFsMetadata=false）
        bool    bHasFsMetadata = false;
        qint64  nCreatedTime = 0;      // UTC 毫秒，0 = 未知
        qint64  nModifiedTime = 0;
        qint64  nAccessedTime = 0;
        quint32 nAttributes = 0;       // FILE_ATTRIBUTE_* 的 R/H/S/A 位
    };

    explicit DlgFileProperty(const Data &data, QWidget *parent = nullptr);

    // 供「复制信息」与单元测试使用：每个字段一行的纯文本
    static QString formatInfoText(const Data &data);
    // "22.37 KB（22,907 字节）"；小于 1 KB 时只写字节。独立出来便于单测。
    static QString formatSize(qint64 nBytes);
    // 类型文案："DOCX 文件"；没有后缀时为 "文件"
    static QString typeTextForFile(const QString &sFileName);

private:
    struct Section
    {
        QString sTitle;
        QVector<QPair<QString, QString>> rows;
    };
    static QVector<Section> buildSections(const Data &data);

    Data m_data;
};

#endif // DLGFILEPROPERTY_H

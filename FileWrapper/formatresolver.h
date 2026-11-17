#ifndef FORMATRESOLVER_H
#define FORMATRESOLVER_H

#include <QByteArray>
#include <QString>
#include <QVector>

// ============================================================================
// FormatResolver：判断"这个文件到底是什么格式"，以及"能不能用现有插件打开"。
//
// 为什么需要：用户拿到的文件经常后缀不可靠（下载改名、无后缀、.dat/.bin），
// 而"插件映射"要求用户先知道该选哪个插件 —— 对普通用户来说等于没给办法。
// 这里按**文件头**识别真实格式（docx/xlsx/pptx/odt/ods/odp/epub/pdf/图片/
// 音视频/压缩包/旧版 Office 等），能交给现有插件时直接给插件后缀；
// 不能处理时给出可照做的建议（另存为 .docx、用某类工具打开……）。
//
// 只读文件头（调用方传前 256 KB 即可）：OOXML/ODF 的关键标志都出现在文件开头
// 的本地文件头/首条目里，不需要解析整个 ZIP。
// ============================================================================
namespace FormatResolver
{
struct Detection
{
    QString formatId;        // 机器可读 id（"png" / "docx" / "ole2" / "zip" / "unknown" ...）
    QString displayName;     // 中文名称（给用户看）
    QString pluginExt;       // 能交给现有插件时：对应后缀（如 "png"）；空 = 没有插件能处理
    QString advice;          // 用户可照做的建议（另存为 / 用别的方式打开 …）
    QString detail;          // 判断依据（"文件头 %PDF-" / "ZIP 内含 xl/workbook.xml"）
    bool    fromContent = false;
};

// 按扩展名判断（不带点、小写）
Detection byExtension(const QString &ext);

// 按文件头判断（head 为文件开头若干字节，建议 256 KB）
Detection byContent(const QByteArray &head);

// 综合判断：内容优先（更可靠），内容判断不出来时再看扩展名
Detection detect(const QString &fileName, const QByteArray &head);

// 「常见但暂不支持」的格式清单：扩展名 -> 建议（给「查看支持的格式」对话框用）
QVector<QPair<QString, QString>> knownUnsupported();

} // namespace FormatResolver

#endif // FORMATRESOLVER_H

#include "formatresolver.h"

#include <QFileInfo>
#include <QHash>
#include <QStringList>

#include <cstring>

namespace {

using FormatResolver::Detection;

Detection make(const QString &id, const QString &name, const QString &pluginExt,
               const QString &advice, const QString &detail, bool fromContent)
{
    Detection d;
    d.formatId = id;
    d.displayName = name;
    d.pluginExt = pluginExt;
    d.advice = advice;
    d.detail = detail;
    d.fromContent = fromContent;
    return d;
}

bool hasPrefix(const QByteArray &d, const char *bytes, int n)
{
    return d.size() >= n && std::memcmp(d.constData(), bytes, size_t(n)) == 0;
}

// 在文件头里找 ASCII 标志（忽略大小写）。
// 旧实现的注释写着「忽略大小写」，实际是区分大小写的 indexOf：ZIP 中央目录里
// 部件名的大小写并不总与规范一致（尤其第三方生成的 docx），会误判成未知 ZIP。
bool containsAscii(const QByteArray &d, const char *needle)
{
    const QByteArray hay = d.toLower();
    return hay.indexOf(QByteArray(needle).toLower()) >= 0;
}

QString extOf(const QString &fileName)
{
    return QFileInfo(fileName).suffix().toLower();
}

// MPEG-TS 没有统一魔数：靠 188 字节定长包里每包首字节 0x47 同步字节判断。
// M2TS 是 192 字节（包前多 4 字节到达时间戳），带 FEC 的变体是 204 字节。
//
// 'G' == 0x47，纯文本里非常常见，所以只看"每 188 字节一个 G"不够 —— 还要看包头的
// 合法位：transport_error_indicator 必须为 0，adaptation_field_control 不能是保留值 00。
// 两条件一起用，普通源码/文本（行首 G、注释里的 G）不会被误判成视频。
bool tsSyncAt(const QByteArray &d, qsizetype pos)
{
    if (pos < 0 || pos + 4 > d.size()) return false;
    if (static_cast<quint8>(d.at(pos)) != 0x47) return false;
    const quint8 b1 = static_cast<quint8>(d.at(pos + 1));
    const quint8 b3 = static_cast<quint8>(d.at(pos + 3));
    if ((b1 & 0x80) != 0) return false;       // transport_error_indicator = 1：不是有效包头
    if ((b3 & 0x30) == 0) return false;       // adaptation_field_control = 00：保留值
    return true;
}

// 返回识别出的包长（188/192/204），0 = 不是 MPEG-TS。
int mpegTsStride(const QByteArray &d)
{
    static const struct { int nStride; int nSyncOffset; } kVariants[] = {
        { 188, 0 },     // 原生 MPEG-TS（.ts）
        { 192, 4 },     // M2TS / AVCHD（.m2ts）
        { 204, 0 },     // 带 RS(204,188) FEC 的 TS
    };
    for (const auto &v : kVariants)
    {
        // 至少连续 4 个包边界同步：单个 0x47 在任意二进制里都有 1/256 的概率出现。
        if (d.size() < v.nSyncOffset + v.nStride * 3 + 4)
            continue;
        bool bAllSync = true;
        for (int i = 0; i < 4 && bAllSync; ++i)
            bAllSync = tsSyncAt(d, v.nSyncOffset + qsizetype(i) * v.nStride);
        if (bAllSync)
            return v.nStride;
    }
    return 0;
}

} // namespace

namespace FormatResolver
{

Detection byExtension(const QString &ext)
{
    const QString e = ext.toLower();

    // ---- 常见但本程序不处理：给出可照做的建议 ----
    static const QHash<QString, QPair<QString, QString>> kUnsupported = {
        // 旧版 Office 二进制
        { QStringLiteral("doc"),  { QStringLiteral("旧版 Word 文档（.doc）"),
                                    QStringLiteral("请用 Word / WPS 打开后「另存为」.docx，再重新打开。") } },
        { QStringLiteral("xls"),  { QStringLiteral("旧版 Excel 表格（.xls）"),
                                    QStringLiteral("请用 Excel / WPS 打开后「另存为」.xlsx。") } },
        { QStringLiteral("ppt"),  { QStringLiteral("旧版 PowerPoint 演示（.ppt）"),
                                    QStringLiteral("请用 PowerPoint / WPS 打开后「另存为」.pptx。") } },
        { QStringLiteral("pps"),  { QStringLiteral("旧版 PowerPoint 放映（.pps）"),
                                    QStringLiteral("请另存为 .pptx 后重试。") } },
        // WPS / 国产办公
        { QStringLiteral("wps"),  { QStringLiteral("WPS 文字文档"),
                                    QStringLiteral("请用 WPS 打开后另存为 .docx。") } },
        { QStringLiteral("et"),   { QStringLiteral("WPS 表格"),
                                    QStringLiteral("请用 WPS 打开后另存为 .xlsx。") } },
        { QStringLiteral("dps"),  { QStringLiteral("WPS 演示"),
                                    QStringLiteral("请用 WPS 打开后另存为 .pptx。") } },
        { QStringLiteral("ofd"),  { QStringLiteral("OFD 版式文档（国标）"),
                                    QStringLiteral("请用 OFD 阅读器打开；本程序只支持流式文档与表格/演示。") } },
        { QStringLiteral("caj"),  { QStringLiteral("知网 CAJ 文献"),
                                    QStringLiteral("请用 CAJViewer 打开。") } },
        { QStringLiteral("djvu"), { QStringLiteral("DjVu 文档"),
                                    QStringLiteral("请用 DjVu 阅读器打开。") } },
        { QStringLiteral("chm"),  { QStringLiteral("Windows 帮助文档（.chm）"),
                                    QStringLiteral("请用系统帮助查看器打开。") } },
        { QStringLiteral("xps"),  { QStringLiteral("XPS 文档"),
                                    QStringLiteral("可用 Windows 的 XPS 查看器打开。") } },
        { QStringLiteral("oxps"), { QStringLiteral("OpenXPS 文档"),
                                    QStringLiteral("可用 Windows 的 XPS 查看器打开。") } },
        // 图像
        { QStringLiteral("psd"),  { QStringLiteral("Photoshop 工程文件"),
                                    QStringLiteral("请用 Photoshop/GIMP 导出为 PNG/JPEG 后查看。") } },
        { QStringLiteral("heic"), { QStringLiteral("HEIF/HEIC 照片"),
                                    QStringLiteral("可用系统「照片」应用打开并另存为 JPEG。") } },
        { QStringLiteral("heif"), { QStringLiteral("HEIF/HEIC 照片"),
                                    QStringLiteral("可用系统「照片」应用打开并另存为 JPEG。") } },
        { QStringLiteral("avif"), { QStringLiteral("AVIF 图像"),
                                    QStringLiteral("请转换为 PNG/JPEG 后查看。") } },
        { QStringLiteral("raw"),  { QStringLiteral("相机 RAW 原始照片"),
                                    QStringLiteral("请用相机厂商软件导出为 JPEG/TIFF。") } },
        { QStringLiteral("cr2"),  { QStringLiteral("佳能 RAW 照片"),
                                    QStringLiteral("请用相机厂商软件导出为 JPEG/TIFF。") } },
        { QStringLiteral("nef"),  { QStringLiteral("尼康 RAW 照片"),
                                    QStringLiteral("请用相机厂商软件导出为 JPEG/TIFF。") } },
        // 音视频（未接线的封装）
        { QStringLiteral("mid"),  { QStringLiteral("MIDI 音乐序列（不是录音）"),
                                    QStringLiteral("请用 MIDI 播放器/编曲软件打开。") } },
        { QStringLiteral("midi"), { QStringLiteral("MIDI 音乐序列（不是录音）"),
                                    QStringLiteral("请用 MIDI 播放器/编曲软件打开。") } },
        // 容器类
        { QStringLiteral("zip"),  { QStringLiteral("ZIP 压缩包"),
                                    QStringLiteral("本程序是文件归档工具，不能直接浏览压缩包；请先用解压软件展开。") } },
        { QStringLiteral("rar"),  { QStringLiteral("RAR 压缩包"),
                                    QStringLiteral("请先用解压软件展开。") } },
        { QStringLiteral("7z"),   { QStringLiteral("7z 压缩包"),
                                    QStringLiteral("请先用解压软件展开。") } },
        { QStringLiteral("tar"),  { QStringLiteral("TAR 归档"),
                                    QStringLiteral("请先用解压软件展开。") } },
        { QStringLiteral("gz"),   { QStringLiteral("GZip 压缩文件"),
                                    QStringLiteral("请先用解压软件展开。") } },
        // 其它
        { QStringLiteral("eml"),  { QStringLiteral("邮件文件（.eml）"),
                                    QStringLiteral("请用邮件客户端打开。") } },
        { QStringLiteral("msg"),  { QStringLiteral("Outlook 邮件（.msg）"),
                                    QStringLiteral("请用 Outlook 打开。") } },
        { QStringLiteral("sqlite"), { QStringLiteral("SQLite 数据库"),
                                      QStringLiteral("请用数据库工具打开。") } },
        { QStringLiteral("db"),   { QStringLiteral("数据库文件"),
                                    QStringLiteral("请用对应的数据库工具打开。") } },
    };

    auto it = kUnsupported.constFind(e);
    if (it != kUnsupported.constEnd())
        return make(e, it->first, QString(), it->second,
                    QStringLiteral("按扩展名 .%1 判断").arg(e), false);

    // ---- 明确支持的格式（仅用于给用户展示名称；路由本身走插件声明） ----
    static const QHash<QString, QString> kSupported = {
        { QStringLiteral("pdf"),   QStringLiteral("PDF 文档") },
        { QStringLiteral("docx"),  QStringLiteral("Word 文档（.docx）") },
        { QStringLiteral("docm"),  QStringLiteral("Word 宏文档") },
        { QStringLiteral("dotx"),  QStringLiteral("Word 模板") },
        { QStringLiteral("odt"),   QStringLiteral("OpenDocument 文字") },
        { QStringLiteral("rtf"),   QStringLiteral("RTF 富文本") },
        { QStringLiteral("xlsx"),  QStringLiteral("Excel 表格") },
        { QStringLiteral("xlsm"),  QStringLiteral("Excel 宏表格") },
        { QStringLiteral("ods"),   QStringLiteral("OpenDocument 表格") },
        { QStringLiteral("pptx"),  QStringLiteral("PowerPoint 演示") },
        { QStringLiteral("odp"),   QStringLiteral("OpenDocument 演示") },
        { QStringLiteral("epub"),  QStringLiteral("EPUB 电子书") },
        { QStringLiteral("fb2"),   QStringLiteral("FictionBook 电子书") },
        { QStringLiteral("mobi"),  QStringLiteral("Mobipocket 电子书") },
        { QStringLiteral("png"),   QStringLiteral("PNG 图像") },
        { QStringLiteral("jpg"),   QStringLiteral("JPEG 图像") },
        { QStringLiteral("jpeg"),  QStringLiteral("JPEG 图像") },
        { QStringLiteral("gif"),   QStringLiteral("GIF 图像") },
        { QStringLiteral("bmp"),   QStringLiteral("BMP 图像") },
        { QStringLiteral("webp"),  QStringLiteral("WebP 图像") },
        { QStringLiteral("svg"),   QStringLiteral("SVG 矢量图") },
        { QStringLiteral("tiff"),  QStringLiteral("TIFF 图像") },
        { QStringLiteral("ico"),   QStringLiteral("ICO 图标") },
        { QStringLiteral("mp4"),   QStringLiteral("MP4 视频") },
        { QStringLiteral("mkv"),   QStringLiteral("MKV 视频") },
        { QStringLiteral("avi"),   QStringLiteral("AVI 视频") },
        { QStringLiteral("mov"),   QStringLiteral("QuickTime 视频") },
        { QStringLiteral("mp3"),   QStringLiteral("MP3 音频") },
        { QStringLiteral("wav"),   QStringLiteral("WAV 音频") },
        { QStringLiteral("flac"),  QStringLiteral("FLAC 音频") },
        { QStringLiteral("txt"),   QStringLiteral("纯文本") },
        { QStringLiteral("md"),    QStringLiteral("Markdown 文本") },
        { QStringLiteral("html"),  QStringLiteral("HTML 网页") },
    };

    auto sit = kSupported.constFind(e);
    if (sit != kSupported.constEnd())
        return make(e, sit.value(), e, QString(),
                    QStringLiteral("按扩展名 .%1 判断").arg(e), false);

    return Detection();   // 未知：交给内容判断 / 对话框
}

Detection byContent(const QByteArray &head)
{
    if (head.isEmpty()) return Detection();

    // ---- 有明确魔数的格式 ----
    if (hasPrefix(head, "%PDF-", 5))
        return make(QStringLiteral("pdf"), QStringLiteral("PDF 文档"), QStringLiteral("pdf"),
                    QString(), QStringLiteral("文件头 %PDF-"), true);

    if (hasPrefix(head, "{\\rtf", 5))
        return make(QStringLiteral("rtf"), QStringLiteral("RTF 富文本"), QStringLiteral("rtf"),
                    QString(), QStringLiteral("文件头 {\\rtf"), true);

    if (hasPrefix(head, "\x89PNG\r\n\x1a\n", 8))
        return make(QStringLiteral("png"), QStringLiteral("PNG 图像"), QStringLiteral("png"),
                    QString(), QStringLiteral("文件头 PNG"), true);

    if (head.size() >= 3
        && static_cast<quint8>(head[0]) == 0xFF
        && static_cast<quint8>(head[1]) == 0xD8
        && static_cast<quint8>(head[2]) == 0xFF)
        return make(QStringLiteral("jpg"), QStringLiteral("JPEG 图像"), QStringLiteral("jpg"),
                    QString(), QStringLiteral("文件头 FFD8FF"), true);

    if (head.startsWith("GIF87a") || head.startsWith("GIF89a"))
        return make(QStringLiteral("gif"), QStringLiteral("GIF 图像"), QStringLiteral("gif"),
                    QString(), QStringLiteral("文件头 GIF"), true);

    // TIFF：II*\0（小端）或 MM\0*（大端）。.tif/.tiff 都是图片插件声明的后缀。
    if (hasPrefix(head, "II*\0", 4) || hasPrefix(head, "MM\0*", 4))
        return make(QStringLiteral("tiff"), QStringLiteral("TIFF 图像"), QStringLiteral("tiff"),
                    QString(), QStringLiteral("文件头 II*/MM*（TIFF）"), true);

    if (hasPrefix(head, "BM", 2) && head.size() >= 10
        && head[6] == 0 && head[7] == 0 && head[8] == 0 && head[9] == 0)
        return make(QStringLiteral("bmp"), QStringLiteral("BMP 图像"), QStringLiteral("bmp"),
                    QString(), QStringLiteral("文件头 BM"), true);

    if (hasPrefix(head, "RIFF", 4) && head.size() >= 12)
    {
        const QByteArray kind = head.mid(8, 4);
        if (kind == "WEBP")
            return make(QStringLiteral("webp"), QStringLiteral("WebP 图像"), QStringLiteral("webp"),
                        QString(), QStringLiteral("文件头 RIFF/WEBP"), true);
        if (kind == "WAVE")
            return make(QStringLiteral("wav"), QStringLiteral("WAV 音频"), QStringLiteral("wav"),
                        QString(), QStringLiteral("文件头 RIFF/WAVE"), true);
        if (kind == "AVI ")
            return make(QStringLiteral("avi"), QStringLiteral("AVI 视频"), QStringLiteral("avi"),
                        QString(), QStringLiteral("文件头 RIFF/AVI"), true);
    }

    if (hasPrefix(head, "8BPS", 4))
        return make(QStringLiteral("psd"), QStringLiteral("Photoshop 工程文件（.psd）"), QString(),
                    QStringLiteral("请用 Photoshop/GIMP 导出为 PNG/JPEG 后查看。"),
                    QStringLiteral("文件头 8BPS"), true);

    // 旧版 Office：OLE2 复合文档（.doc/.xls/.ppt 之一）
    if (hasPrefix(head, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8))
        return make(QStringLiteral("ole2"), QStringLiteral("旧版 Office 二进制文档（.doc/.xls/.ppt 之一）"),
                    QString(),
                    QStringLiteral("请用 Word / Excel / PowerPoint 或 WPS 打开后，"
                                   "另存为 .docx / .xlsx / .pptx 再重新打开。"),
                    QStringLiteral("文件头 D0CF11E0（OLE2 复合文档）"), true);

    // ZIP 家族：按包内标志判断是哪一种文档
    if (hasPrefix(head, "PK\x03\x04", 4) || head.startsWith("PK\x05\x06"))
    {
        if (containsAscii(head, "word/document.xml"))
            return make(QStringLiteral("docx"), QStringLiteral("Word 文档（.docx）"),
                        QStringLiteral("docx"), QString(),
                        QStringLiteral("ZIP 内含 word/document.xml"), true);
        if (containsAscii(head, "xl/workbook.xml"))
            return make(QStringLiteral("xlsx"), QStringLiteral("Excel 表格（.xlsx）"),
                        QStringLiteral("xlsx"), QString(),
                        QStringLiteral("ZIP 内含 xl/workbook.xml"), true);
        if (containsAscii(head, "ppt/presentation.xml"))
            return make(QStringLiteral("pptx"), QStringLiteral("PowerPoint 演示（.pptx）"),
                        QStringLiteral("pptx"), QString(),
                        QStringLiteral("ZIP 内含 ppt/presentation.xml"), true);
        if (containsAscii(head, "application/vnd.oasis.opendocument.text"))
            return make(QStringLiteral("odt"), QStringLiteral("OpenDocument 文字（.odt）"),
                        QStringLiteral("odt"), QString(),
                        QStringLiteral("ZIP mimetype 为 OpenDocument Text"), true);
        if (containsAscii(head, "application/vnd.oasis.opendocument.spreadsheet"))
            return make(QStringLiteral("ods"), QStringLiteral("OpenDocument 表格（.ods）"),
                        QStringLiteral("ods"), QString(),
                        QStringLiteral("ZIP mimetype 为 OpenDocument Spreadsheet"), true);
        if (containsAscii(head, "application/vnd.oasis.opendocument.presentation"))
            return make(QStringLiteral("odp"), QStringLiteral("OpenDocument 演示（.odp）"),
                        QStringLiteral("odp"), QString(),
                        QStringLiteral("ZIP mimetype 为 OpenDocument Presentation"), true);
        if (containsAscii(head, "application/epub+zip"))
            return make(QStringLiteral("epub"), QStringLiteral("EPUB 电子书"),
                        QStringLiteral("epub"), QString(),
                        QStringLiteral("ZIP mimetype 为 application/epub+zip"), true);

        return make(QStringLiteral("zip"), QStringLiteral("ZIP 压缩包或未知 ZIP 文档"), QString(),
                    QStringLiteral("如果是 Office/电子书文档，请确认后缀（.docx/.xlsx/.pptx/.odt/.epub）；"
                                   "否则请先用解压软件展开。"),
                    QStringLiteral("文件头 PK（ZIP 容器）"), true);
    }

    // 音视频魔数
    if (hasPrefix(head, "ID3", 3))
        return make(QStringLiteral("mp3"), QStringLiteral("MP3 音频"), QStringLiteral("mp3"),
                    QString(), QStringLiteral("文件头 ID3"), true);
    if (hasPrefix(head, "\xFF\xFB", 2) || hasPrefix(head, "\xFF\xF3", 2))
        return make(QStringLiteral("mp3"), QStringLiteral("MP3 音频"), QStringLiteral("mp3"),
                    QString(), QStringLiteral("文件头 MPEG 音频帧"), true);
    if (hasPrefix(head, "fLaC", 4))
        return make(QStringLiteral("flac"), QStringLiteral("FLAC 音频"), QStringLiteral("flac"),
                    QString(), QStringLiteral("文件头 fLaC"), true);
    if (hasPrefix(head, "OggS", 4))
        return make(QStringLiteral("ogg"), QStringLiteral("Ogg 音频"), QStringLiteral("ogg"),
                    QString(), QStringLiteral("文件头 OggS"), true);
    if (hasPrefix(head, "\x1A\x45\xDF\xA3", 4))
        return make(QStringLiteral("mkv"), QStringLiteral("Matroska/WebM 视频"), QStringLiteral("mkv"),
                    QString(), QStringLiteral("文件头 EBML"), true);
    if (head.size() >= 12 && head.mid(4, 4) == "ftyp")
    {
        // P2-1：ftyp 不是「MP4 专用」。先看 major brand，再看兼容 brand 列表，
        // 避免把 HEIC/AVIF 照片误判成 MP4 视频（交给错误的插件打开）。
        const QByteArray major = head.mid(8, 4).toLower();
        QByteArray brands = major;
        for (int off = 16; off + 4 <= head.size() && off < 16 + 4 * 16; off += 4)
            brands += ' ' + head.mid(off, 4).toLower();

        if (brands.contains("heic") || brands.contains("heix") || brands.contains("heim")
            || brands.contains("heis") || brands.contains("hevc") || brands.contains("hevx"))
            return make(QStringLiteral("heic"), QStringLiteral("HEIF/HEIC 照片"), QString(),
                        QStringLiteral("可用系统「照片」应用打开并另存为 JPEG。"),
                        QStringLiteral("ftyp brand 为 %1").arg(QString::fromLatin1(major)), true);
        if (brands.contains("avif") || brands.contains("avis"))
            return make(QStringLiteral("avif"), QStringLiteral("AVIF 图像"), QString(),
                        QStringLiteral("请转换为 PNG/JPEG 后查看。"),
                        QStringLiteral("ftyp brand 为 %1").arg(QString::fromLatin1(major)), true);
        if (brands.contains("qt  "))
            return make(QStringLiteral("mov"), QStringLiteral("QuickTime 视频"), QStringLiteral("mov"),
                        QString(), QStringLiteral("ftyp brand 为 %1").arg(QString::fromLatin1(major)), true);
        if (brands.contains("3gp") || brands.contains("3g2"))
            return make(QStringLiteral("mp4"), QStringLiteral("3GP 视频"), QStringLiteral("mp4"),
                        QString(), QStringLiteral("ftyp brand 为 %1").arg(QString::fromLatin1(major)), true);

        // 其余常见 ISO BMFF 视频品牌（isom/mp41/mp42/avc1/M4V/…）按 MP4 处理
        return make(QStringLiteral("mp4"), QStringLiteral("MP4/MOV 视频"), QStringLiteral("mp4"),
                    QString(), QStringLiteral("ftyp brand 为 %1").arg(QString::fromLatin1(major)), true);
    }

    // MPEG-TS：pluginExt 故意给 "m2ts" 而不是 "ts"。
    // 后缀 ".ts" 在插件注册表里归 PluginText（TypeScript 源码的优先级更高），
    // 返回 "ts" 会被路由回文本插件，视频又被灌成乱码；"m2ts" 由 PluginMedia 声明，
    // 两者是同一个解码后端，只是绕开了后缀歧义。
    if (const int nTsStride = mpegTsStride(head))
        return make(QStringLiteral("mpegts"), QStringLiteral("MPEG-TS 视频（.ts/.m2ts）"),
                    QStringLiteral("m2ts"), QString(),
                    QStringLiteral("每 %1 字节出现 0x47 同步字节，且包头合法").arg(nTsStride), true);

    // 压缩包
    if (hasPrefix(head, "7z\xBC\xAF\x27\x1C", 6))
        return make(QStringLiteral("7z"), QStringLiteral("7z 压缩包"), QString(),
                    QStringLiteral("本程序不能直接浏览压缩包；请先用解压软件展开。"),
                    QStringLiteral("文件头 7z"), true);
    if (hasPrefix(head, "Rar!", 4))
        return make(QStringLiteral("rar"), QStringLiteral("RAR 压缩包"), QString(),
                    QStringLiteral("请先用解压软件展开。"), QStringLiteral("文件头 Rar!"), true);
    if (hasPrefix(head, "\x1F\x8B", 2))
        return make(QStringLiteral("gz"), QStringLiteral("GZip 压缩文件"), QString(),
                    QStringLiteral("请先用解压软件展开。"), QStringLiteral("文件头 1F8B"), true);
    if (head.size() > 262 && head.mid(257, 5) == "ustar")
        return make(QStringLiteral("tar"), QStringLiteral("TAR 归档"), QString(),
                    QStringLiteral("请先用解压软件展开。"), QStringLiteral("偏移 257 处 ustar"), true);

    // 可执行 / 数据库 / 文本类
    if (hasPrefix(head, "MZ", 2))
        return make(QStringLiteral("exe"), QStringLiteral("Windows 可执行文件"), QString(),
                    QStringLiteral("可执行文件不能作为文档查看。"), QStringLiteral("文件头 MZ"), true);
    if (head.startsWith("SQLite format 3"))
        return make(QStringLiteral("sqlite"), QStringLiteral("SQLite 数据库"), QString(),
                    QStringLiteral("请用数据库工具打开。"), QStringLiteral("文件头 SQLite format 3"), true);

    {
        const QByteArray trimmed = head.left(4096).trimmed().toLower();
        if (trimmed.startsWith("<!doctype html") || trimmed.startsWith("<html"))
            return make(QStringLiteral("html"), QStringLiteral("HTML 网页"), QStringLiteral("html"),
                        QString(), QStringLiteral("内容以 <!DOCTYPE html 开头"), true);

        // SVG / FictionBook 也是 XML，必须先于通用 xml 分支识别：否则它们会被判成
        // 「XML 文档」交给文本插件，图片/电子书插件永远轮不到（改名后的 .svg/.fb2 尤其明显）。
        // 只在开头 8 KB 里找根元素；HTML 里可能内联 <svg>，所以先排除 HTML。
        const QByteArray probe = head.left(8192).toLower();
        if (probe.contains("<fictionbook"))
            return make(QStringLiteral("fb2"), QStringLiteral("FictionBook 电子书"),
                        QStringLiteral("fb2"), QString(),
                        QStringLiteral("XML 根元素为 <FictionBook>"), true);
        if (!probe.contains("<html") && probe.contains("<svg"))
            return make(QStringLiteral("svg"), QStringLiteral("SVG 矢量图"), QStringLiteral("svg"),
                        QString(), QStringLiteral("XML 根元素为 <svg>"), true);

        if (trimmed.startsWith("<?xml"))
            return make(QStringLiteral("xml"), QStringLiteral("XML 文档"), QStringLiteral("xml"),
                        QString(), QStringLiteral("内容以 <?xml 开头"), true);
    }

    return Detection();
}

Detection detect(const QString &fileName, const QByteArray &head)
{
    // 内容优先：后缀可以被随便改，文件头不会
    Detection byContentResult = byContent(head);
    if (!byContentResult.formatId.isEmpty())
        return byContentResult;

    Detection byExtResult = byExtension(extOf(fileName));
    if (!byExtResult.formatId.isEmpty())
        return byExtResult;

    Detection d;
    d.formatId = QStringLiteral("unknown");
    d.displayName = QStringLiteral("未知格式");
    d.advice = QStringLiteral("无法识别这个文件的真实格式。可以先用「以文本查看」看看内容；"
                              "如果它其实是一种文档（如 Word/Excel/PDF），请确认后缀是否正确。");
    d.detail = head.isEmpty() ? QStringLiteral("读不到文件内容")
                              : QStringLiteral("文件头既不是常见文档/图片/音视频，扩展名也未登记");
    return d;
}

QVector<QPair<QString, QString>> knownUnsupported()
{
    return {
        { QStringLiteral(".doc / .xls / .ppt"), QStringLiteral("旧版 Office 二进制：请另存为 .docx / .xlsx / .pptx") },
        { QStringLiteral(".wps / .et / .dps"),  QStringLiteral("WPS 格式：请用 WPS 另存为 .docx / .xlsx / .pptx") },
        { QStringLiteral(".ofd"),               QStringLiteral("版式文档：请用 OFD 阅读器") },
        { QStringLiteral(".caj"),               QStringLiteral("知网文献：请用 CAJViewer") },
        { QStringLiteral(".djvu / .chm / .xps"),QStringLiteral("请用对应的阅读器（DjVu / 帮助查看器 / XPS 查看器）") },
        { QStringLiteral(".psd / .heic / .raw"),QStringLiteral("请先导出为 PNG/JPEG") },
        { QStringLiteral(".zip / .rar / .7z"),  QStringLiteral("压缩包：请先用解压软件展开") },
        { QStringLiteral(".eml / .msg"),        QStringLiteral("邮件：请用邮件客户端打开") },
    };
}

} // namespace FormatResolver

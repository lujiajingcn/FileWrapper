#ifndef WORDDOCUMENT_H
#define WORDDOCUMENT_H

#include <QByteArray>
#include <QDomDocument>
#include <QHash>
#include <QImage>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

#include "zipreader.h"
#include "rtfdocument.h"

// ============================================================================
// WordDocument：把 .docx（OOXML / WordprocessingML）转换成 QTextBrowser 能渲染的 HTML。
//
// 设计要点：
//   * 不依赖第三方库：.docx 本身是 ZIP 容器（zipreader 解压），正文是
//     word/document.xml。这里用 QDomDocument 解析，再手工生成 Qt 富文本子集
//     （<p>/<b>/<i>/<u>/<s>/<table>/<img>/<a>/<sub>/<sup> + 少量 CSS）。
//   * 分段处理：open() 只负责解出各个部件与样式表，render() 才生成 HTML。
//     因此「缩放」只需用不同 fontScale 再 render() 一次，不必重新解压。
//   * 所有字号都显式写进 HTML（不依赖继承），缩放结果可预期。
//
// 支持的常见排版：段落/居中/缩进/行距、标题样式（Heading 1-6 与中文「标题 1」）、
// 粗体/斜体/下划线/删除线/上下标/字体/字号/颜色/底纹、编号与项目符号（含中文数字）、
// 表格（合并单元格 gridSpan / vMerge、底纹、边框）、内嵌图片（PNG/JPEG/GIF/BMP）、
// 超链接、制表符、软换行。
//
// 已知不支持（会静默跳过）：脚注尾注正文、页眉页脚、文本框、域代码结果以外的域、
// .doc 旧版二进制格式（由上层给出提示）、修订删除内容。
// ============================================================================
class WordDocument
{
public:
    struct Stats {
        int paragraphs = 0;   // 段落数
        int headings   = 0;   // 其中标题段落数
        int tables     = 0;   // 表格数
        int images     = 0;   // 内嵌图片数（按文件去重）
        int chars      = 0;   // 正文字符数（不含编号前缀）
    };

    struct RenderResult {
        QString html;                    // 可直接交给 QTextBrowser::setHtml
        QMap<QString, QImage> images;    // 图片 URL -> 图像（addResource 用）
        bool truncated = false;          // 是否因过大被截断
    };

    // ODT 的字符/段落属性（与 docx 的 RunProps/ParProps 不复用：命名与语义都不同）
    enum class HAlign { Inherit, Left, Center, Right, Justify };
    struct OdtRun {
        bool    b       = false;
        bool    i       = false;
        bool    u       = false;
        double  sizePt  = 0.0;
        QString color;
        QString fill;
        QString family;
    };
    struct OdtPar {
        HAlign  align    = HAlign::Inherit;
        double  leftPt   = 0.0;
        double  rightPt  = 0.0;
        double  indentPt = 0.0;
        double  beforePt = 0.0;
        double  afterPt  = 0.0;
        QString fill;
    };

    WordDocument();

    // 解析一个 docx/docm/dotx/dotm 容器的字节内容。失败时 error 填入中文原因。
    bool open(const QByteArray &data, QString *error = nullptr);
    void reset();
    bool isOpen() const { return m_ok; }

    // 生成可渲染的 HTML 片段。fontScale=1.0 为原始字号；>1 放大，<1 缩小。
    RenderResult render(double fontScale = 1.0);

    // 图片 URL 前缀。每打开一个新文件由调用方换一个，避免与上一份文档
    // 在同一个 QTextDocument 里缓存的图片 URL 撞车。open() 不会清除它。
    void setUrlPrefix(const QString &prefix) { m_urlPrefix = prefix; }

    const Stats &stats() const { return m_stats; }
    QString     title()  const { return m_title; }
    QString     author() const { return m_author; }
    QString     errorString() const { return m_error; }

private:
    // ---- 文本 / 段落属性（Word 的 rPr / pPr 精简模型）----
    struct RunProps {
        bool    b       = false;   // 粗体
        bool    i       = false;   // 斜体
        bool    u       = false;   // 下划线
        bool    strike  = false;   // 删除线
        bool    sup     = false;   // 上标
        bool    sub     = false;   // 下标
        bool    caps    = false;   // 全大写
        double  sizePt  = 0.0;     // 0 = 未指定（落到文档默认字号）
        QString color;             // #RRGGBB
        QString highlight;         // #RRGGBB 底纹
        QString family;            // 字体名
        void mergeRPr(const QDomElement &rPr);
    };

    struct ParProps {
        HAlign  align        = HAlign::Inherit;
        double  leftPt       = 0.0;
        double  rightPt      = 0.0;
        double  firstLinePt  = 0.0;   // 正数首行缩进，负数悬挂缩进
        double  beforePt     = 0.0;
        double  afterPt      = 0.0;
        QString lineCss;              // 行距的 CSS 值（如 "150%" / "14pt"），空 = 未指定
        QString fill;                 // 段落底纹
        bool    pageBreakBefore = false;
        int     outlineLvl   = -1;    // -1 = 未指定，0..8 有效
        bool    hasNum       = false;
        int     numId        = -1;
        int     ilvl         = 0;
        void mergePPr(const QDomElement &pPr);
    };

    struct NumberLevel {
        QString numFmt = QStringLiteral("decimal");
        QString lvlText;
        int     start  = 1;
    };
    struct NumberDef {
        QMap<int, NumberLevel> levels;   // ilvl -> 层级定义
    };

    // ---- 各部件解析 ----
    bool loadMainPart(const QString &path);
    void loadRels(const QString &mainPart);
    void loadStyles(const QString &mainPart);
    void loadNumbering(const QString &mainPart);
    void loadCoreProps();
    bool loadXmlPart(const QString &path, QDomDocument *doc) const;

    // ---- 样式查询 ----
    QVector<QDomElement> styleChain(const QString &styleId) const;
    QDomElement          styleElement(const QString &styleId) const;
    static QString       styleName(const QDomElement &style);
    RunProps  baseRunProps(const QString &parStyleId) const;
    ParProps  baseParProps(const QString &parStyleId) const;
    int       headingLevelOf(const QDomElement &p, const QString &parStyleId) const;
    bool      tableHasBorders(const QDomElement &tbl) const;

    // ---- 编号 ----
    QString nextListPrefix(int numId, int ilvl);

    // ---- HTML 生成 ----
    void emitBlocks(const QDomElement &parent, QStringList &out, double scale, int depth);
    void emitParagraph(const QDomElement &p, QStringList &out, double scale);
    void emitRuns(const QDomElement &parent, QStringList &out, const RunProps &base,
                  double scale, const QString &linkTarget);
    void emitRun(const QDomElement &r, QStringList &out, const RunProps &base,
                 double scale, const QString &linkTarget);
    void emitTable(const QDomElement &tbl, QStringList &out, double scale, int depth);
    void emitAlternateContent(const QDomElement &ac, QStringList &out, const RunProps &base,
                              double scale, const QString &linkTarget);
    QString imageTag(const QDomElement &container, double scale);

    // ---- ODT（OpenDocument Text）：与 docx 共用 ZipReader 与图片资源机制 ----
    // ODT 与 docx 都是 ZIP+XML，但元素/属性命名完全不同（text:p / style:text-properties…
    // / fo:font-size），这里用独立的轻量 emitter 映射到同一套 HTML 子集：
    // 段落/标题/字符格式/列表/表格/内嵌图片/超链接。缩放仍走 render(fontScale)。
    bool loadOdt(const QByteArray &data, QString *error);
    void emitOdtBody(const QDomElement &parent, QStringList &out, double scale, int depth);
    void emitOdtParagraph(const QDomElement &e, QStringList &out, double scale,
                          bool heading, int headingLevel, int listDepth,
                          const QString &bulletPrefix);
    void emitOdtInline(const QDomElement &parent, QStringList &out, const OdtRun &base,
                       double scale, const QString &link);
    void emitOdtList(const QDomElement &list, QStringList &out, double scale, int depth);
    void emitOdtTable(const QDomElement &table, QStringList &out, double scale, int depth);
    QString odtImageTag(const QDomElement &frame, double scale);
    OdtRun  odtRunProps(const QString &styleName) const;
    OdtPar  odtParProps(const QString &styleName) const;
    QString odtWrapRun(const OdtRun &r, double scale, const QString &html) const;
    static OdtRun odtMergeRun(const OdtRun &base, const OdtRun &over);
    static double odtLenToPt(const QString &value);

    bool         m_isOdt = false;
    bool         m_isRtf = false;
    RtfDocument  m_rtf;                         // .rtf 子集解析（段落/字符/对齐/缩进）
    QDomDocument m_odtContent;                  // content.xml
    QDomElement  m_odtText;                     // office:text
    QDomDocument m_odtStyleDoc;                 // styles.xml（可选）
    QMap<QString, QDomElement> m_odtStyles;     // style:name -> style:style
    QMap<QString, QString>     m_odtFontFaces;  // style:font-name -> 字体族
    QMap<QString, bool>        m_odtListOrdered; // 列表样式名 -> 是否有序

    QString m_error;

    // ---- 容器与部件 ----
    ZipReader   m_zip;           // 持有整个容器的字节副本
    QDomDocument m_doc;          // 主文档部件（word/document.xml）
    QDomElement  m_body;         // w:body
    QDomDocument m_styles;       // word/styles.xml
    QDomElement  m_stylesRoot;
    QDomDocument m_numbering;    // word/numbering.xml
    QMap<QString, QDomElement> m_styleById;
    QMap<QString, QString>     m_docRels;      // rId -> Target（相对主文档目录）
    QMap<int, int>             m_numToAbstract; // numId -> abstractNumId
    QMap<int, NumberDef>       m_abstractNums;
    QHash<int, QVector<int>>   m_numCounters;   // numId -> 各层级当前计数
    QString m_mainPartDir;                      // 例如 "word/"
    QString m_title;
    QString m_author;

    // ---- 渲染期状态 ----
    double m_defaultSizePt = 10.5;   // 文档默认字号（五号），可由 docDefaults 覆盖
    Stats  m_stats;
    bool   m_truncated = false;
    int    m_blockBudget = 0;
    int    m_imageSeq = 0;
    QString m_urlPrefix = QStringLiteral("wimg://img/");   // 图片 URL 前缀（见 setUrlPrefix）
    QMap<QString, QString> m_targetToUrl;   // zip 内图片路径 -> wimg:// URL（去重）
    QMap<QString, QImage>  m_imageCache;    // URL -> 已解码图像（缩放重排时复用）
    bool   m_ok = false;
};

#endif // WORDDOCUMENT_H

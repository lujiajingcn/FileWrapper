#include "worddocument.h"

#include <QDomElement>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QtGlobal>

// ============================================================================
// 实现说明
//
// docx = ZIP（OPC 容器）+ XML：
//   _rels/.rels                     -> 指向主文档部件（通常 word/document.xml）
//   word/document.xml               -> 正文（w:body 下的 w:p / w:tbl ...）
//   word/_rels/document.xml.rels    -> rId 到图片 / 超链接目标的映射
//   word/styles.xml                 -> 段落样式、字符样式、文档默认字号
//   word/numbering.xml              -> 编号 / 项目符号定义
//   docProps/core.xml               -> 标题、作者
//
// 解析时 **不开启命名空间处理**（QDomDocument::ParseOption::Default）：
// 这样 tagName 会保留 "w:p" / "a:blip" 这类前缀，直接按前缀比较即可，
// 不必为每个版本的前缀声明建 URI 表；写法也与 XML 原文一一对应，便于排查。
// ============================================================================

namespace {

// ---- 限制（防御畸形 / 恶意文件）----
constexpr int    kMaxBlockCount   = 100000;          // 段落 + 表格总量上限
constexpr int    kMaxDepth        = 6;               // 表格 / AlternateContent 递归深度上限
constexpr qint64 kMaxImageBytes   = 32 * 1024 * 1024; // 单张内嵌图片上限
constexpr double kMaxImageDisplayW = 1400.0;          // 图片显示宽度上限（px）
constexpr double kListIndentPt    = 18.0;             // 列表每级缩进（pt）
constexpr double kTwipsPerPt      = 20.0;
constexpr double kEmuPerPx        = 9525.0;
// Qt 6.9.3 的富文本导入器只认 px 的 margin-* / text-indent：
// 「margin-left:18pt」会被**静默忽略**（实测 leftMargin=0），换成 24px 才生效。
// 因此这些属性统一按 px 输出（1pt = 96/72 px）。
constexpr double kPtToPx = 96.0 / 72.0;

// ---- 文本 / 属性辅助 ----

// HTML 文本转义
QString esc(const QString &s)
{
    QString r;
    r.reserve(s.size() + 16);
    for (const QChar ch : s)
    {
        switch (ch.unicode())
        {
        case '&':  r += QLatin1String("&amp;");  break;
        case '<':  r += QLatin1String("&lt;");   break;
        case '>':  r += QLatin1String("&gt;");   break;
        default:   r += ch;                      break;
        }
    }
    return r;
}

// HTML 属性值转义（额外的引号）
QString escAttr(const QString &s)
{
    QString r = esc(s);
    r.replace(QLatin1Char('"'), QLatin1String("&quot;"));
    r.replace(QLatin1Char('\''), QLatin1String("&#39;"));
    return r;
}

// Word 用 "FF0000" 表示颜色；"auto" / 空 / 非法值一律视为未指定
QString cssColor(const QString &v)
{
    const QString t = v.trimmed();
    if (t.size() != 6) return QString();
    for (const QChar ch : t)
        if (!ch.isDigit() && !(ch.toLower() >= QLatin1Char('a') && ch.toLower() <= QLatin1Char('f')))
            return QString();
    return QLatin1Char('#') + t.toUpper();
}

// twips（1/20 pt）字符串 -> pt
double twipsToPt(const QString &v)
{
    bool ok = false;
    const double n = v.trimmed().toDouble(&ok);
    return ok ? n / kTwipsPerPt : 0.0;
}

// CSS 长度（"12pt" / "1.5in" / "2cm" / "30mm" / "100px"）-> px
double parseLengthPx(const QString &v)
{
    QString s = v.trimmed().toLower();
    if (s.isEmpty()) return 0.0;
    double factor = 1.0;                       // 默认按 px
    if (s.endsWith(QLatin1String("pt")))      { factor = 96.0 / 72.0; s.chop(2); }
    else if (s.endsWith(QLatin1String("in"))) { factor = 96.0;        s.chop(2); }
    else if (s.endsWith(QLatin1String("cm"))) { factor = 96.0 / 2.54; s.chop(2); }
    else if (s.endsWith(QLatin1String("mm"))) { factor = 96.0 / 25.4; s.chop(2); }
    else if (s.endsWith(QLatin1String("px"))) { factor = 1.0;         s.chop(2); }
    else if (s.endsWith(QLatin1String("em"))) { factor = 16.0;        s.chop(2); }
    bool ok = false;
    const double n = s.trimmed().toDouble(&ok);
    return ok ? n * factor : 0.0;
}

// 数字 -> 去掉多余小数位的字符串（用于 CSS）
QString fmtNum(double v)
{
    QString s = QString::number(v, 'f', 2);
    while (s.endsWith(QLatin1Char('0'))) s.chop(1);
    if (s.endsWith(QLatin1Char('.')))     s.chop(1);
    return s;
}

QDomElement firstChild(const QDomElement &el, const QString &tag)
{
    return el.isNull() ? QDomElement() : el.firstChildElement(tag);
}

// 取子元素的 w:val（不存在则空串）
QString childVal(const QDomElement &el, const QString &tag)
{
    const QDomElement c = firstChild(el, tag);
    return c.isNull() ? QString() : c.attribute(QStringLiteral("w:val"));
}

// 深度优先查找后代元素
QDomElement findDescendant(const QDomElement &el, const QString &tag, int depth = 0)
{
    if (el.isNull() || depth > kMaxDepth) return QDomElement();
    for (QDomElement c = el.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
    {
        if (c.tagName() == tag) return c;
        const QDomElement r = findDescendant(c, tag, depth + 1);
        if (!r.isNull()) return r;
    }
    return QDomElement();
}

// <w:b/> 这类开关属性：无属性=on，"0/false/off/none"=off
bool toggleValue(const QDomElement &e)
{
    if (e.isNull()) return false;
    if (!e.hasAttribute(QStringLiteral("w:val"))) return true;
    const QString v = e.attribute(QStringLiteral("w:val")).trimmed().toLower();
    return !(v == QLatin1String("0") || v == QLatin1String("false")
             || v == QLatin1String("off") || v == QLatin1String("none"));
}

// 字体名只保留安全字符，避免内容里的怪字符破坏 style 属性
QString sanitizeFontFamily(const QString &f)
{
    QString r;
    const QString t = f.trimmed();
    for (const QChar ch : t)
    {
        if (ch.isLetterOrNumber() || ch == QLatin1Char(' ') || ch == QLatin1Char('-')
            || ch == QLatin1Char('_') || ch.isMark() || ch.unicode() > 0x2000)
            r += ch;
    }
    return r.trimmed();
}

// 只放行常见协议，避免 docx 里的 javascript: 之类链接被点击执行
QString safeHref(const QString &target)
{
    const QString t = target.trimmed();
    if (t.isEmpty()) return QString();
    const int colon = t.indexOf(QLatin1Char(':'));
    if (colon > 0)
    {
        const QString scheme = t.left(colon).toLower();
        bool looksLikeScheme = !scheme.isEmpty();
        for (const QChar ch : scheme)
        {
            if (!(ch.isLetterOrNumber() || ch == QLatin1Char('+')
                  || ch == QLatin1Char('-') || ch == QLatin1Char('.')))
            {
                looksLikeScheme = false;
                break;
            }
        }
        if (looksLikeScheme)
        {
            static const QStringList kAllowed = {
                QStringLiteral("http"), QStringLiteral("https"),
                QStringLiteral("ftp"),  QStringLiteral("mailto"),
                QStringLiteral("file"), QStringLiteral("tel")
            };
            if (!kAllowed.contains(scheme)) return QString();
        }
    }
    return t;
}

// HTML 会折叠连续空格：首尾与连续出现的空格换成不换行空格，保住排版
QString preserveSpaces(const QString &in)
{
    QString out;
    out.reserve(in.size() + 8);
    const QChar nbsp(0x00A0);
    const int n = in.size();
    for (int i = 0; i < n; ++i)
    {
        const QChar ch = in.at(i);
        if (ch != QLatin1Char(' ')) { out += ch; continue; }
        const bool prevSpace = i > 0 && in.at(i - 1) == QLatin1Char(' ');
        const bool nextSpace = i + 1 < n && in.at(i + 1) == QLatin1Char(' ');
        const bool atEdge    = i == 0 || i + 1 == n;
        out += (prevSpace || nextSpace || atEdge) ? nbsp : ch;
    }
    return out;
}

// 中文数字（1-99，编号格式 chineseCounting 等）
QString chineseNumber(int n)
{
    static const char *kDigits[10] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
    if (n <= 0 || n >= 100) return QString::number(n);
    if (n < 10)  return QString::fromUtf8(kDigits[n]);
    if (n == 10) return QStringLiteral("十");
    if (n < 20)  return QStringLiteral("十") + QString::fromUtf8(kDigits[n % 10]);
    QString s = QString::fromUtf8(kDigits[n / 10]) + QStringLiteral("十");
    if (n % 10) s += QString::fromUtf8(kDigits[n % 10]);
    return s;
}

QString romanNumber(int n, bool upper)
{
    if (n <= 0 || n > 3999) return QString::number(n);
    static const int kVal[13] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
    static const char *kSym[13] = {"m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i"};
    QString s;
    for (int i = 0; i < 13; ++i)
        while (n >= kVal[i]) { s += QLatin1String(kSym[i]); n -= kVal[i]; }
    return upper ? s.toUpper() : s;
}

QString alphaNumber(int n, bool upper)
{
    if (n <= 0) return QString::number(n);
    const QChar c = QChar(QLatin1Char('a').unicode() + (n - 1) % 26);
    return QString(upper ? c.toUpper() : c);
}

// 按 Word 的 numFmt 格式化一个计数
QString formatCounter(int value, const QString &numFmt)
{
    const QString f = numFmt.trimmed().toLower();
    if (f == QLatin1String("lowerletter") || f == QLatin1String("lowercaseletter"))
        return alphaNumber(value, false);
    if (f == QLatin1String("upperletter") || f == QLatin1String("uppercaseletter"))
        return alphaNumber(value, true);
    if (f == QLatin1String("lowerroman")) return romanNumber(value, false);
    if (f == QLatin1String("upperroman")) return romanNumber(value, true);
    if (f == QLatin1String("decimalzero"))
        return QStringLiteral("%1").arg(value, 2, 10, QLatin1Char('0'));
    // 中文 / 日文 / 韩文的「一二三」类编号
    if (f.contains(QLatin1String("chinese")) || f.contains(QLatin1String("ideograph"))
        || f.contains(QLatin1String("japanese")) || f.contains(QLatin1String("taiwanese"))
        || f.contains(QLatin1String("korean")))
        return chineseNumber(value);
    return QString::number(value);
}

// 高亮（w:highlight）颜色名 -> #RRGGBB
QString highlightColor(const QString &name)
{
    const QString n = name.trimmed().toLower();
    static const QHash<QString, QString> kMap = {
        { QStringLiteral("black"),       QStringLiteral("#000000") },
        { QStringLiteral("blue"),        QStringLiteral("#0000FF") },
        { QStringLiteral("cyan"),        QStringLiteral("#00FFFF") },
        { QStringLiteral("green"),       QStringLiteral("#00FF00") },
        { QStringLiteral("magenta"),     QStringLiteral("#FF00FF") },
        { QStringLiteral("red"),         QStringLiteral("#FF0000") },
        { QStringLiteral("yellow"),      QStringLiteral("#FFFF00") },
        { QStringLiteral("white"),       QStringLiteral("#FFFFFF") },
        { QStringLiteral("darkblue"),    QStringLiteral("#000080") },
        { QStringLiteral("darkcyan"),    QStringLiteral("#008080") },
        { QStringLiteral("darkgreen"),   QStringLiteral("#008000") },
        { QStringLiteral("darkmagenta"), QStringLiteral("#800080") },
        { QStringLiteral("darkred"),     QStringLiteral("#800000") },
        { QStringLiteral("darkyellow"),  QStringLiteral("#808000") },
        { QStringLiteral("darkgray"),    QStringLiteral("#808080") },
        { QStringLiteral("lightgray"),   QStringLiteral("#C0C0C0") }
    };
    return kMap.value(n);
}

// 目录部分路径（"word/document.xml" -> "word/"）
QString dirOf(const QString &path)
{
    const int i = path.lastIndexOf(QLatin1Char('/'));
    return i < 0 ? QString() : path.left(i + 1);
}

QString baseNameOf(const QString &path)
{
    const int i = path.lastIndexOf(QLatin1Char('/'));
    return i < 0 ? path : path.mid(i + 1);
}

// OPC 部件名归一化：去掉前导 '/'，反斜杠转 '/'
QString normalizePart(const QString &p)
{
    QString r = p.trimmed();
    r.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (r.startsWith(QLatin1Char('/'))) r.remove(0, 1);
    return r;
}

// 相对主文档目录解析部件路径（"media/image1.png" -> "word/media/image1.png"）
QString resolvePart(const QString &baseDir, const QString &target)
{
    QString t = normalizePart(target);
    if (t.contains(QLatin1String("://"))) return QString();   // 外部链接
    QStringList parts = baseDir.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    const QStringList segs = t.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &seg : segs)
    {
        if (seg == QLatin1String(".")) continue;
        if (seg == QLatin1String("..")) { if (!parts.isEmpty()) parts.removeLast(); continue; }
        parts.append(seg);
    }
    return parts.join(QLatin1Char('/'));
}

} // namespace

// ============================================================================
// RunProps / ParProps
// ============================================================================
void WordDocument::RunProps::mergeRPr(const QDomElement &rPr)
{
    if (rPr.isNull()) return;

    // 粗体 / 斜体（bCs、iCs 是复杂文种（中文等）的对应属性，含义相同）
    if (!firstChild(rPr, QStringLiteral("w:b")).isNull())
        b = toggleValue(firstChild(rPr, QStringLiteral("w:b")));
    else if (!firstChild(rPr, QStringLiteral("w:bCs")).isNull())
        b = toggleValue(firstChild(rPr, QStringLiteral("w:bCs")));

    if (!firstChild(rPr, QStringLiteral("w:i")).isNull())
        i = toggleValue(firstChild(rPr, QStringLiteral("w:i")));
    else if (!firstChild(rPr, QStringLiteral("w:iCs")).isNull())
        i = toggleValue(firstChild(rPr, QStringLiteral("w:iCs")));

    // 下划线：w:u w:val="none" 表示无下划线
    const QDomElement uEl = firstChild(rPr, QStringLiteral("w:u"));
    if (!uEl.isNull())
    {
        const QString v = uEl.attribute(QStringLiteral("w:val")).trimmed().toLower();
        u = !(v == QLatin1String("none") || v == QLatin1String("0") || v == QLatin1String("false"));
    }

    if (!firstChild(rPr, QStringLiteral("w:strike")).isNull())
        strike = toggleValue(firstChild(rPr, QStringLiteral("w:strike")));
    else if (!firstChild(rPr, QStringLiteral("w:dstrike")).isNull())
        strike = toggleValue(firstChild(rPr, QStringLiteral("w:dstrike")));

    if (!firstChild(rPr, QStringLiteral("w:caps")).isNull())
        caps = toggleValue(firstChild(rPr, QStringLiteral("w:caps")));

    // 上下标
    const QString va = childVal(rPr, QStringLiteral("w:vertAlign")).trimmed().toLower();
    if (va == QLatin1String("superscript")) { sup = true; sub = false; }
    else if (va == QLatin1String("subscript")) { sub = true; sup = false; }
    else if (va == QLatin1String("baseline")) { sup = sub = false; }

    // 字号（半磅）
    double sz = childVal(rPr, QStringLiteral("w:sz")).toDouble();
    if (sz <= 0) sz = childVal(rPr, QStringLiteral("w:szCs")).toDouble();
    if (sz > 0 && sz <= 400) sizePt = sz / 2.0;   // 上限 200pt，过滤畸形值

    // 颜色
    const QDomElement color = firstChild(rPr, QStringLiteral("w:color"));
    if (!color.isNull())
    {
        const QString c = cssColor(color.attribute(QStringLiteral("w:val")));
        if (!c.isEmpty()) this->color = c;
    }

    // 底纹 / 高亮
    const QDomElement shd = firstChild(rPr, QStringLiteral("w:shd"));
    if (!shd.isNull())
    {
        const QString c = cssColor(shd.attribute(QStringLiteral("w:fill")));
        if (!c.isEmpty() && c != QLatin1String("#FFFFFF")) highlight = c;
    }
    const QDomElement hl = firstChild(rPr, QStringLiteral("w:highlight"));
    if (!hl.isNull())
    {
        const QString c = highlightColor(hl.attribute(QStringLiteral("w:val")));
        if (!c.isEmpty()) highlight = c;
    }

    // 字体
    const QDomElement fonts = firstChild(rPr, QStringLiteral("w:rFonts"));
    if (!fonts.isNull())
    {
        QString fam = fonts.attribute(QStringLiteral("w:ascii"));
        if (fam.isEmpty()) fam = fonts.attribute(QStringLiteral("w:eastAsia"));
        if (fam.isEmpty()) fam = fonts.attribute(QStringLiteral("w:hAnsi"));
        fam = sanitizeFontFamily(fam);
        if (!fam.isEmpty()) family = fam;
    }
}

void WordDocument::ParProps::mergePPr(const QDomElement &pPr)
{
    if (pPr.isNull()) return;

    const QDomElement jc = firstChild(pPr, QStringLiteral("w:jc"));
    if (!jc.isNull())
    {
        const QString v = jc.attribute(QStringLiteral("w:val")).trimmed().toLower();
        if (v == QLatin1String("left") || v == QLatin1String("start"))        align = HAlign::Left;
        else if (v == QLatin1String("center"))                                 align = HAlign::Center;
        else if (v == QLatin1String("right") || v == QLatin1String("end"))     align = HAlign::Right;
        else if (v == QLatin1String("both") || v == QLatin1String("distribute")
                 || v == QLatin1String("thaiDistribute"))                      align = HAlign::Justify;
    }

    const QDomElement ind = firstChild(pPr, QStringLiteral("w:ind"));
    if (!ind.isNull())
    {
        if (ind.hasAttribute(QStringLiteral("w:left")))
            leftPt = twipsToPt(ind.attribute(QStringLiteral("w:left")));
        else if (ind.hasAttribute(QStringLiteral("w:start")))
            leftPt = twipsToPt(ind.attribute(QStringLiteral("w:start")));

        if (ind.hasAttribute(QStringLiteral("w:right")))
            rightPt = twipsToPt(ind.attribute(QStringLiteral("w:right")));
        else if (ind.hasAttribute(QStringLiteral("w:end")))
            rightPt = twipsToPt(ind.attribute(QStringLiteral("w:end")));

        if (ind.hasAttribute(QStringLiteral("w:hanging")))
            firstLinePt = -twipsToPt(ind.attribute(QStringLiteral("w:hanging")));
        else if (ind.hasAttribute(QStringLiteral("w:firstLine")))
            firstLinePt = twipsToPt(ind.attribute(QStringLiteral("w:firstLine")));
    }

    const QDomElement spacing = firstChild(pPr, QStringLiteral("w:spacing"));
    if (!spacing.isNull())
    {
        if (spacing.hasAttribute(QStringLiteral("w:before")))
            beforePt = twipsToPt(spacing.attribute(QStringLiteral("w:before")));
        if (spacing.hasAttribute(QStringLiteral("w:after")))
            afterPt = twipsToPt(spacing.attribute(QStringLiteral("w:after")));

        const double line = spacing.attribute(QStringLiteral("w:line")).toDouble();
        const QString rule = spacing.attribute(QStringLiteral("w:lineRule")).trimmed().toLower();
        if (line > 0)
        {
            if (rule.isEmpty() || rule == QLatin1String("auto"))
                lineCss = QStringLiteral("%1%").arg(fmtNum(line / 240.0 * 100.0));
            else
                lineCss = QStringLiteral("%1pt").arg(fmtNum(line / kTwipsPerPt));
        }
    }

    const QDomElement pshd = firstChild(pPr, QStringLiteral("w:shd"));
    if (!pshd.isNull())
    {
        const QString c = cssColor(pshd.attribute(QStringLiteral("w:fill")));
        if (!c.isEmpty() && c != QLatin1String("#FFFFFF")) fill = c;
    }

    const QDomElement pbb = firstChild(pPr, QStringLiteral("w:pageBreakBefore"));
    if (!pbb.isNull()) pageBreakBefore = toggleValue(pbb);

    const QDomElement outline = firstChild(pPr, QStringLiteral("w:outlineLvl"));
    if (!outline.isNull())
    {
        const int lvl = outline.attribute(QStringLiteral("w:val")).toInt();
        if (lvl >= 0 && lvl <= 8) outlineLvl = lvl;
    }

    const QDomElement numPr = firstChild(pPr, QStringLiteral("w:numPr"));
    if (!numPr.isNull())
    {
        const QDomElement numIdEl = firstChild(numPr, QStringLiteral("w:numId"));
        if (!numIdEl.isNull())
        {
            const int id = numIdEl.attribute(QStringLiteral("w:val")).toInt();
            if (id > 0) { hasNum = true; numId = id; }
            else        { hasNum = false; numId = -1; }   // numId=0 表示取消编号
        }
        const QDomElement ilvlEl = firstChild(numPr, QStringLiteral("w:ilvl"));
        if (!ilvlEl.isNull())
        {
            const int l = ilvlEl.attribute(QStringLiteral("w:val")).toInt();
            ilvl = qBound(0, l, 8);
        }
    }
}

// ============================================================================
// 打开与部件解析
// ============================================================================
WordDocument::WordDocument() = default;

void WordDocument::reset()
{
    m_error.clear();
    m_zip = ZipReader();
    m_doc = QDomDocument();
    m_body = QDomElement();
    m_styles = QDomDocument();
    m_stylesRoot = QDomElement();
    m_numbering = QDomDocument();
    m_styleById.clear();
    m_docRels.clear();
    m_numToAbstract.clear();
    m_abstractNums.clear();
    m_numCounters.clear();
    m_mainPartDir.clear();
    m_title.clear();
    m_author.clear();
    m_defaultSizePt = 10.5;
    m_stats = Stats();
    m_truncated = false;
    m_blockBudget = 0;
    m_imageSeq = 0;
    m_targetToUrl.clear();
    m_imageCache.clear();
    m_rtf.reset();
    m_isRtf = false;
    m_isOdt = false;
    m_odtContent = QDomDocument();
    m_odtText = QDomElement();
    m_odtStyleDoc = QDomDocument();
    m_odtStyles.clear();
    m_odtFontFaces.clear();
    m_odtListOrdered.clear();
    m_ok = false;
}

bool WordDocument::loadXmlPart(const QString &path, QDomDocument *doc) const
{
    if (path.isEmpty()) return false;
    const QByteArray bytes = m_zip.entry(path);
    if (bytes.isEmpty()) return false;
    const QDomDocument::ParseResult pr = doc->setContent(bytes);
    return static_cast<bool>(pr);
}

bool WordDocument::open(const QByteArray &data, QString *error)
{
    reset();
    auto fail = [&](const QString &msg) {
        m_error = msg;
        if (error) *error = msg;
        return false;
    };

    if (data.isEmpty())
        return fail(QStringLiteral("文件为空（0 字节），没有可显示的内容。"));

    // ---- 旧版格式要有明确提示，而不是笼统的「解析失败」----
    if (data.size() >= 8
        && static_cast<quint8>(data[0]) == 0xD0 && static_cast<quint8>(data[1]) == 0xCF
        && static_cast<quint8>(data[2]) == 0x11 && static_cast<quint8>(data[3]) == 0xE0)
    {
        return fail(QStringLiteral(
            "这是旧版 Word 二进制格式（.doc，OLE2 复合文档），不是 .docx。\n\n"
            "该格式的正文由分片存储与快速保存机制组织，解析复杂度与格式风险都很高，"
            "本插件不做支持。\n\n"
            "请用 Word / WPS 打开后「另存为」.docx，再重新打开即可正常显示。"));
    }
    {
        const QByteArray head = data.left(5);
        if (head == QByteArrayLiteral("{\\rtf"))
        {
            if (!m_rtf.open(data, &m_error))
                return fail(m_error.isEmpty()
                                ? QStringLiteral("无法解析该 RTF 文档。") : m_error);
            m_isRtf = true;
            m_ok = true;
            m_title = QStringLiteral("RTF 文档");
            return true;
        }
    }

    if (!m_zip.open(data))
    {
        return fail(QStringLiteral(
            "无法解析该文件：它不是有效的 .docx（ZIP 容器读取失败）。\n\n"
            "可能原因：文件已损坏、下载不完整，或其实是别的格式（如 .doc / .rtf）。"));
    }

    // ODT（OpenDocument Text）同样是 ZIP 容器：用 mimetype 条目区分（OOXML 没有这个条目）。
    if (m_zip.entry(QStringLiteral("mimetype")).contains("opendocument.text"))
        return loadOdt(data, error);

    // ---- 1) 通过 _rels/.rels 找到主文档部件 ----
    QString mainPart;
    QDomDocument rootRels;
    if (loadXmlPart(QStringLiteral("_rels/.rels"), &rootRels))
    {
        for (QDomElement e = rootRels.documentElement().firstChildElement(QStringLiteral("Relationship"));
             !e.isNull(); e = e.nextSiblingElement(QStringLiteral("Relationship")))
        {
            if (e.attribute(QStringLiteral("Type")).endsWith(QLatin1String("/officeDocument")))
            {
                mainPart = normalizePart(e.attribute(QStringLiteral("Target")));
                break;
            }
        }
    }
    if (mainPart.isEmpty())
        mainPart = QStringLiteral("word/document.xml");

    if (!loadMainPart(mainPart))
    {
        if (m_zip.contains(QStringLiteral("word/document.xml")))
            mainPart = QStringLiteral("word/document.xml");
        else
            return fail(QStringLiteral(
                "这个文件是 ZIP 容器，但里面没有 Word 正文部件（word/document.xml）。\n\n"
                "它可能不是 .docx（例如 .xlsx / .pptx / .odt / 普通压缩包），"
                "或者文件结构已损坏。"));
        if (!loadMainPart(mainPart))
            return fail(m_error.isEmpty()
                        ? QStringLiteral("Word 正文部件解析失败，文件可能已损坏。")
                        : m_error);
    }

    m_mainPartDir = dirOf(mainPart);

    // ---- 2) 关系 / 样式 / 编号 / 文档属性 ----
    loadRels(mainPart);
    loadStyles(mainPart);
    loadNumbering(mainPart);
    loadCoreProps();

    if (m_body.isNull())
        return fail(QStringLiteral("Word 正文部件里没有 <w:body>，无法显示内容。"));

    m_ok = true;
    if (error) error->clear();
    return true;
}

bool WordDocument::loadMainPart(const QString &path)
{
    const QByteArray bytes = m_zip.entry(path);
    if (bytes.isEmpty())
    {
        // 条目存在但解不出（超过单条目解压上限 / 压缩方式不支持）时要说明白，
        // 否则「没有找到正文部件」会误导成文件不是 docx。
        m_error = m_zip.contains(path)
                ? QStringLiteral("正文部件体积过大或压缩方式不受支持，无法解压：%1").arg(path)
                : QStringLiteral("没有找到 Word 正文部件：%1").arg(path);
        return false;
    }

    QDomDocument doc;
    const QDomDocument::ParseResult pr = doc.setContent(bytes);
    if (!pr)
    {
        m_error = QStringLiteral("正文 XML 解析失败：%1（第 %2 行）")
                      .arg(pr.errorMessage).arg(pr.errorLine);
        return false;
    }

    const QDomElement root = doc.documentElement();
    if (root.tagName() != QLatin1String("w:document"))
    {
        m_error = QStringLiteral("正文根节点不是 <w:document>（实际为 <%1>）。").arg(root.tagName());
        return false;
    }

    QDomElement body = root.firstChildElement(QStringLiteral("w:body"));
    if (body.isNull())
    {
        m_error = QStringLiteral("正文里没有 <w:body> 节点。");
        return false;
    }

    m_doc = doc;        // QDomDocument 隐式共享，元素在 m_doc 存活期间有效
    m_body = body;
    return true;
}

void WordDocument::loadRels(const QString &mainPart)
{
    const QString relsPath = dirOf(mainPart) + QStringLiteral("_rels/")
                             + baseNameOf(mainPart) + QStringLiteral(".rels");
    QDomDocument doc;
    if (!loadXmlPart(relsPath, &doc)) return;

    for (QDomElement e = doc.documentElement().firstChildElement(QStringLiteral("Relationship"));
         !e.isNull(); e = e.nextSiblingElement(QStringLiteral("Relationship")))
    {
        const QString id     = e.attribute(QStringLiteral("Id"));
        const QString target = e.attribute(QStringLiteral("Target"));
        if (id.isEmpty() || target.isEmpty()) continue;
        m_docRels.insert(id, target);
    }
}

void WordDocument::loadStyles(const QString &mainPart)
{
    QDomDocument doc;
    if (!loadXmlPart(dirOf(mainPart) + QStringLiteral("styles.xml"), &doc)) return;

    m_styles = doc;
    m_stylesRoot = doc.documentElement();

    // 文档默认字号：docDefaults/rPrDefault/rPr/w:sz（半磅）
    const QDomElement defaults = m_stylesRoot.firstChildElement(QStringLiteral("w:docDefaults"));
    const QDomElement rPrDefault = firstChild(defaults, QStringLiteral("w:rPrDefault"));
    const QDomElement rPr = firstChild(rPrDefault, QStringLiteral("w:rPr"));
    const double sz = childVal(rPr, QStringLiteral("w:sz")).toDouble();
    if (sz > 0 && sz <= 400) m_defaultSizePt = sz / 2.0;

    for (QDomElement st = m_stylesRoot.firstChildElement(QStringLiteral("w:style"));
         !st.isNull(); st = st.nextSiblingElement(QStringLiteral("w:style")))
    {
        const QString id = st.attribute(QStringLiteral("w:styleId"));
        if (!id.isEmpty()) m_styleById.insert(id, st);
    }
}

void WordDocument::loadNumbering(const QString &mainPart)
{
    QDomDocument doc;
    if (!loadXmlPart(dirOf(mainPart) + QStringLiteral("numbering.xml"), &doc)) return;

    m_numbering = doc;
    const QDomElement root = doc.documentElement();

    for (QDomElement an = root.firstChildElement(QStringLiteral("w:abstractNum"));
         !an.isNull(); an = an.nextSiblingElement(QStringLiteral("w:abstractNum")))
    {
        bool ok = false;
        const int absId = an.attribute(QStringLiteral("w:abstractNumId")).toInt(&ok);
        if (!ok) continue;

        NumberDef def;
        for (QDomElement lvl = an.firstChildElement(QStringLiteral("w:lvl"));
             !lvl.isNull(); lvl = lvl.nextSiblingElement(QStringLiteral("w:lvl")))
        {
            const int ilvl = lvl.attribute(QStringLiteral("w:ilvl")).toInt();
            if (ilvl < 0 || ilvl > 8) continue;

            NumberLevel nl;
            nl.numFmt = childVal(lvl, QStringLiteral("w:numFmt")).trimmed().toLower();
            if (nl.numFmt.isEmpty()) nl.numFmt = QStringLiteral("decimal");
            nl.lvlText = childVal(lvl, QStringLiteral("w:lvlText"));
            const int start = childVal(lvl, QStringLiteral("w:start")).toInt();
            nl.start = start > 0 ? start : 1;
            def.levels.insert(ilvl, nl);
        }
        m_abstractNums.insert(absId, def);
    }

    for (QDomElement n = root.firstChildElement(QStringLiteral("w:num"));
         !n.isNull(); n = n.nextSiblingElement(QStringLiteral("w:num")))
    {
        bool ok = false;
        const int numId = n.attribute(QStringLiteral("w:numId")).toInt(&ok);
        if (!ok) continue;
        const int absId = childVal(n, QStringLiteral("w:abstractNumId")).toInt();
        m_numToAbstract.insert(numId, absId);
    }
}

void WordDocument::loadCoreProps()
{
    QDomDocument doc;
    if (!loadXmlPart(QStringLiteral("docProps/core.xml"), &doc)) return;
    const QDomElement root = doc.documentElement();
    // 未开启命名空间处理，tagName 直接就是 "dc:title" / "dc:creator"
    const QString title = root.firstChildElement(QStringLiteral("dc:title")).text().trimmed();
    const QString who   = root.firstChildElement(QStringLiteral("dc:creator")).text().trimmed();
    if (!title.isEmpty()) m_title  = title;
    if (!who.isEmpty())   m_author = who;
}

// ============================================================================
// 样式查询
// ============================================================================
QDomElement WordDocument::styleElement(const QString &styleId) const
{
    if (styleId.isEmpty()) return QDomElement();
    return m_styleById.value(styleId, QDomElement());
}

QString WordDocument::styleName(const QDomElement &style)
{
    if (style.isNull()) return QString();
    const QString n = childVal(style, QStringLiteral("w:name"));
    return n.isEmpty() ? style.attribute(QStringLiteral("w:styleId")) : n;
}

// 返回从最基础样式到 styleId 自身的链（含 basedOn 展开，最多 8 层防环）
QVector<QDomElement> WordDocument::styleChain(const QString &styleId) const
{
    QVector<QDomElement> chain;
    QString id = styleId;
    QSet<QString> seen;
    while (!id.isEmpty() && !seen.contains(id) && chain.size() < 8)
    {
        seen.insert(id);
        const QDomElement st = styleElement(id);
        if (st.isNull()) break;
        chain.prepend(st);
        id = childVal(st, QStringLiteral("w:basedOn"));
    }
    return chain;
}

WordDocument::RunProps WordDocument::baseRunProps(const QString &parStyleId) const
{
    RunProps rp;
    // 文档默认
    if (!m_stylesRoot.isNull())
    {
        const QDomElement defaults = m_stylesRoot.firstChildElement(QStringLiteral("w:docDefaults"));
        const QDomElement rpr = firstChild(firstChild(defaults, QStringLiteral("w:rPrDefault")),
                                           QStringLiteral("w:rPr"));
        rp.mergeRPr(rpr);
    }
    // 段落样式（含 basedOn 链）
    for (const QDomElement &st : styleChain(parStyleId))
        rp.mergeRPr(firstChild(st, QStringLiteral("w:rPr")));
    return rp;
}

WordDocument::ParProps WordDocument::baseParProps(const QString &parStyleId) const
{
    ParProps pp;
    if (!m_stylesRoot.isNull())
    {
        const QDomElement defaults = m_stylesRoot.firstChildElement(QStringLiteral("w:docDefaults"));
        const QDomElement ppr = firstChild(firstChild(defaults, QStringLiteral("w:pPrDefault")),
                                           QStringLiteral("w:pPr"));
        pp.mergePPr(ppr);
    }
    for (const QDomElement &st : styleChain(parStyleId))
        pp.mergePPr(firstChild(st, QStringLiteral("w:pPr")));
    return pp;
}

int WordDocument::headingLevelOf(const QDomElement &p, const QString &parStyleId) const
{
    // 1) outlineLvl 最可靠（Word 的标题样式都带，中文文档也一样）
    const ParProps base = baseParProps(parStyleId);
    const QDomElement pPr = p.firstChildElement(QStringLiteral("w:pPr"));
    ParProps own;
    own.outlineLvl = -1;
    own.mergePPr(pPr);
    int lvl = own.outlineLvl >= 0 ? own.outlineLvl : base.outlineLvl;
    if (lvl >= 0 && lvl <= 5) return lvl + 1;   // 0..5 -> h1..h6

    // 2) 样式名 / 样式 ID 匹配 "heading 1"、"标题 1"、"Heading1"
    static const QRegularExpression reName(QStringLiteral("^(?:heading|标题)\\s*([1-6])$"));
    static const QRegularExpression reId(QStringLiteral("^heading\\s*([1-6])$"));
    for (const QDomElement &st : styleChain(parStyleId))
    {
        const QString name = styleName(st).trimmed().toLower();
        const QRegularExpressionMatch m = reName.match(name);
        if (m.hasMatch()) return m.captured(1).toInt();

        const QString id = st.attribute(QStringLiteral("w:styleId")).trimmed().toLower();
        const QRegularExpressionMatch m2 = reId.match(id);
        if (m2.hasMatch()) return m2.captured(1).toInt();
    }
    return 0;
}

bool WordDocument::tableHasBorders(const QDomElement &tbl) const
{
    // 返回 true 表示表格有边框；Word 的表格边框可能写在 tblPr 里，
    // 也可能只写在表格样式中，两处都要看。
    auto bordersState = [](const QDomElement &borders) -> int {
        if (borders.isNull()) return -1;          // 未定义
        static const QStringList kEdges = {
            QStringLiteral("w:top"), QStringLiteral("w:left"),
            QStringLiteral("w:bottom"), QStringLiteral("w:right"),
            QStringLiteral("w:insideH"), QStringLiteral("w:insideV")
        };
        for (const QString &edge : kEdges)
        {
            const QDomElement e = borders.firstChildElement(edge);
            if (e.isNull()) continue;
            const QString v = e.attribute(QStringLiteral("w:val")).trimmed().toLower();
            if (!v.isEmpty() && v != QLatin1String("none") && v != QLatin1String("nil"))
                return 1;
        }
        return 0;
    };

    const QDomElement tblPr = tbl.firstChildElement(QStringLiteral("w:tblPr"));
    const int direct = bordersState(firstChild(tblPr, QStringLiteral("w:tblBorders")));
    if (direct >= 0) return direct == 1;

    const QString styleId = childVal(tblPr, QStringLiteral("w:tblStyle"));
    for (const QDomElement &st : styleChain(styleId))
    {
        const int s = bordersState(firstChild(firstChild(st, QStringLiteral("w:tblPr")),
                                              QStringLiteral("w:tblBorders")));
        if (s >= 0) return s == 1;
    }
    return false;
}

// ============================================================================
// 编号
// ============================================================================
QString WordDocument::nextListPrefix(int numId, int ilvl)
{
    const auto numIt = m_numToAbstract.constFind(numId);
    if (numIt == m_numToAbstract.constEnd()) return QString();
    const auto absIt = m_abstractNums.constFind(numIt.value());
    if (absIt == m_abstractNums.constEnd()) return QString();

    const NumberDef def = absIt.value();
    ilvl = qBound(0, ilvl, 8);
    if (!def.levels.contains(ilvl)) return QString();

    QVector<int> &c = m_numCounters[numId];
    if (c.size() < 9) c.resize(9);            // 0 表示该层还没用过
    for (int j = 0; j < ilvl; ++j)
        if (c[j] <= 0) c[j] = def.levels.contains(j) ? def.levels[j].start : 1;

    const NumberLevel &lvl = def.levels[ilvl];
    c[ilvl] = (c[ilvl] <= 0) ? lvl.start : c[ilvl] + 1;
    for (int j = ilvl + 1; j < c.size(); ++j) c[j] = 0;   // 更深层归零

    if (lvl.numFmt == QLatin1String("bullet"))
        return QStringLiteral("•");
    if (lvl.numFmt == QLatin1String("none"))
        return QString();

    QString text = lvl.lvlText;
    if (text.isEmpty()) text = QStringLiteral("%1.");
    for (int k = 1; k <= 9; ++k)
    {
        const QString token = QStringLiteral("%%1").arg(k);
        if (!text.contains(token)) continue;
        const int level = k - 1;
        int value = level < c.size() ? c[level] : 0;
        if (value <= 0) value = def.levels.contains(level) ? def.levels[level].start : 1;
        const QString fmt = def.levels.contains(level) ? def.levels[level].numFmt
                                                       : QStringLiteral("decimal");
        text.replace(token, formatCounter(value, fmt));
    }
    return text;
}

// ============================================================================
// HTML 生成
// ============================================================================
WordDocument::RenderResult WordDocument::render(double fontScale)
{
    RenderResult res;
    if (!m_ok) return res;
    if (!(fontScale > 0.2 && fontScale < 8.0)) fontScale = 1.0;   // 同时挡住 NaN

    m_stats = Stats();
    m_numCounters.clear();
    m_truncated = false;
    m_blockBudget = 0;

    // RTF 走自己的轻量解析器：直接产出 HTML，不需要 ZipReader / 样式表
    if (m_isRtf)
    {
        RenderResult rtfRes;
        rtfRes.html = m_rtf.render(fontScale);
        m_stats.paragraphs = m_rtf.paragraphs().size();
        m_stats.chars      = m_rtf.charCount();
        m_stats.images     = 0;
        rtfRes.truncated   = false;
        return rtfRes;
    }

    QStringList out;
    if (m_isOdt)
        emitOdtBody(m_odtText, out, fontScale, 0);
    else
        emitBlocks(m_body, out, fontScale, 0);

    if (m_truncated)
        out << QStringLiteral("<p><i>（文档过大，仅显示前一部分内容）</i></p>");

    res.html = out.join(QString());
    res.images = m_imageCache;
    res.truncated = m_truncated;
    m_stats.images = m_targetToUrl.size();
    return res;
}

void WordDocument::emitBlocks(const QDomElement &parent, QStringList &out, double scale, int depth)
{
    if (parent.isNull() || depth > kMaxDepth) return;

    for (QDomElement e = parent.firstChildElement(); !e.isNull(); e = e.nextSiblingElement())
    {
        if (m_truncated) return;
        const QString tag = e.tagName();

        if (tag == QLatin1String("w:p"))
        {
            emitParagraph(e, out, scale);
        }
        else if (tag == QLatin1String("w:tbl"))
        {
            emitTable(e, out, scale, depth);
        }
        else if (tag == QLatin1String("w:sdt"))
        {
            const QDomElement content = e.firstChildElement(QStringLiteral("w:sdtContent"));
            if (!content.isNull()) emitBlocks(content, out, scale, depth);
        }
        else if (tag == QLatin1String("w:ins") || tag == QLatin1String("w:moveTo")
                 || tag == QLatin1String("w:customXml") || tag == QLatin1String("w:smartTag"))
        {
            emitBlocks(e, out, scale, depth);
        }
        else if (tag == QLatin1String("mc:AlternateContent"))
        {
            // Word 2010+ 的兼容性包装：优先取 mc:Choice，否则取 mc:Fallback
            QDomElement chosen = e.firstChildElement(QStringLiteral("mc:Choice"));
            if (chosen.isNull()) chosen = e.firstChildElement(QStringLiteral("mc:Fallback"));
            if (!chosen.isNull()) emitBlocks(chosen, out, scale, depth);
        }
        // 其余（w:sectPr / w:bookmarkStart / w:del ...）对页面显示无意义，跳过
    }
}

void WordDocument::emitParagraph(const QDomElement &p, QStringList &out, double scale)
{
    if (++m_blockBudget > kMaxBlockCount) { m_truncated = true; return; }

    const QDomElement pPr = p.firstChildElement(QStringLiteral("w:pPr"));
    const QString parStyleId = childVal(pPr, QStringLiteral("w:pStyle"));

    ParProps pp = baseParProps(parStyleId);
    pp.mergePPr(pPr);                       // 段落自身的属性优先

    const int heading = headingLevelOf(p, parStyleId);

    RunProps base = baseRunProps(parStyleId);
    if (heading > 0 && base.sizePt <= 0.0)
    {
        // 样式表里没给字号（或缺少 styles.xml）时，按标题级别给一个合理的默认字号
        static const double kHeadSize[6] = {20.0, 16.0, 14.0, 12.0, 11.0, 10.5};
        base.sizePt = kHeadSize[qBound(1, heading, 6) - 1];
        base.b = true;
    }

    // 编号 / 项目符号
    QString prefix;
    double listLeft = 0.0;
    double listHanging = 0.0;
    if (pp.hasNum)
    {
        prefix = nextListPrefix(pp.numId, pp.ilvl);
        if (!prefix.isEmpty())
        {
            listLeft = (pp.ilvl + 1) * kListIndentPt;
            listHanging = kListIndentPt;
        }
    }

    QStringList runs;
    emitRuns(p, runs, base, scale, QString());

    ++m_stats.paragraphs;
    if (heading > 0) ++m_stats.headings;

    // ---- 段落 CSS ----
    QStringList css;
    switch (pp.align)
    {
    case HAlign::Left:    css << QStringLiteral("text-align:left");    break;
    case HAlign::Center:  css << QStringLiteral("text-align:center");  break;
    case HAlign::Right:   css << QStringLiteral("text-align:right");   break;
    case HAlign::Justify: css << QStringLiteral("text-align:justify"); break;
    case HAlign::Inherit: break;
    }

    const double left = pp.leftPt + listLeft;
    if (left > 0.0)
        css << QStringLiteral("margin-left:%1px").arg(fmtNum(left * kPtToPx));
    if (pp.rightPt > 0.0)
        css << QStringLiteral("margin-right:%1px").arg(fmtNum(pp.rightPt * kPtToPx));

    double indent = pp.firstLinePt;
    if (!prefix.isEmpty() && indent == 0.0) indent = -listHanging;   // 悬挂缩进对齐编号
    if (indent != 0.0)
        css << QStringLiteral("text-indent:%1px").arg(fmtNum(indent * kPtToPx));

    if (pp.beforePt > 0.0)
        css << QStringLiteral("margin-top:%1px").arg(fmtNum(pp.beforePt * kPtToPx));
    if (pp.afterPt  > 0.0)
        css << QStringLiteral("margin-bottom:%1px").arg(fmtNum(pp.afterPt * kPtToPx));
    if (!pp.lineCss.isEmpty()) css << QStringLiteral("line-height:") + pp.lineCss;
    if (!pp.fill.isEmpty())    css << QStringLiteral("background-color:") + pp.fill;

    const double baseSize = (base.sizePt > 0.0 ? base.sizePt : m_defaultSizePt) * scale;
    css << QStringLiteral("font-size:%1pt").arg(fmtNum(baseSize));

    out << QStringLiteral("<p style=\"") << css.join(QLatin1Char(';')) << QStringLiteral("\">");

    if (!prefix.isEmpty())
    {
        // 编号与正文之间用不换行空格填充到悬挂缩进宽度，尽量贴近 Word 的制表位效果
        double prefixEm = 0.0;
        for (const QChar ch : prefix)
            prefixEm += (ch.unicode() < 0x2E80) ? 0.5 : 1.0;
        const double hangingEm = listHanging / qMax(1.0, baseSize);
        const int pad = qBound(1, int((hangingEm - prefixEm) / 0.25 + 0.5), 12);

        QStringList padList;
        for (int i = 0; i < pad; ++i) padList << QStringLiteral("&#160;");
        out << QStringLiteral("<span>") << esc(prefix) << padList.join(QString())
            << QStringLiteral("</span>");
    }

    out << runs.join(QString());
    out << QStringLiteral("</p>");
}

void WordDocument::emitRuns(const QDomElement &parent, QStringList &out, const RunProps &base,
                            double scale, const QString &linkTarget)
{
    for (QDomElement e = parent.firstChildElement(); !e.isNull(); e = e.nextSiblingElement())
    {
        const QString tag = e.tagName();

        if (tag == QLatin1String("w:r"))
        {
            emitRun(e, out, base, scale, linkTarget);
        }
        else if (tag == QLatin1String("w:hyperlink"))
        {
            QString target = linkTarget;
            const QString rid = e.attribute(QStringLiteral("r:id"));
            if (!rid.isEmpty()) target = m_docRels.value(rid, linkTarget);
            target = safeHref(target);

            QStringList sub;
            emitRuns(e, sub, base, scale, target);
            if (!sub.isEmpty() && !target.isEmpty())
                out << QStringLiteral("<a href=\"") << escAttr(target) << QStringLiteral("\">")
                    << sub.join(QString()) << QStringLiteral("</a>");
            else
                out << sub.join(QString());
        }
        else if (tag == QLatin1String("w:ins") || tag == QLatin1String("w:moveTo")
                 || tag == QLatin1String("w:smartTag") || tag == QLatin1String("w:fldSimple")
                 || tag == QLatin1String("w:customXml"))
        {
            emitRuns(e, out, base, scale, linkTarget);
        }
        else if (tag == QLatin1String("w:sdt"))
        {
            const QDomElement content = e.firstChildElement(QStringLiteral("w:sdtContent"));
            if (!content.isNull()) emitRuns(content, out, base, scale, linkTarget);
        }
        else if (tag == QLatin1String("mc:AlternateContent"))
        {
            emitAlternateContent(e, out, base, scale, linkTarget);
        }
        // w:pPr / w:rPr / w:bookmarkStart / w:del / w:proofErr ... 跳过
    }
}

void WordDocument::emitAlternateContent(const QDomElement &ac, QStringList &out,
                                        const RunProps &base, double scale,
                                        const QString &linkTarget)
{
    QDomElement chosen = ac.firstChildElement(QStringLiteral("mc:Choice"));
    if (chosen.isNull()) chosen = ac.firstChildElement(QStringLiteral("mc:Fallback"));
    if (!chosen.isNull()) emitRuns(chosen, out, base, scale, linkTarget);
}

void WordDocument::emitRun(const QDomElement &r, QStringList &out, const RunProps &base,
                           double scale, const QString &linkTarget)
{
    Q_UNUSED(linkTarget);

    // ---- 字符属性：字符样式 -> 直接格式，同一套 rule 依次覆盖 ----
    RunProps rp = base;
    const QDomElement rPr = r.firstChildElement(QStringLiteral("w:rPr"));
    const QString rStyleId = childVal(rPr, QStringLiteral("w:rStyle"));
    for (const QDomElement &st : styleChain(rStyleId))
        rp.mergeRPr(firstChild(st, QStringLiteral("w:rPr")));
    rp.mergeRPr(rPr);

    // ---- 内容片段 ----
    QStringList pieces;
    for (QDomElement e = r.firstChildElement(); !e.isNull(); e = e.nextSiblingElement())
    {
        const QString tag = e.tagName();

        if (tag == QLatin1String("w:t"))
        {
            QString text = e.text();
            if (text.isEmpty()) continue;
            if (rp.caps) text = text.toUpper();
            m_stats.chars += int(text.size());
            pieces << esc(preserveSpaces(text));
        }
        else if (tag == QLatin1String("w:tab"))
        {
            pieces << QStringLiteral("&#160;&#160;&#160;&#160;");   // 近似一个制表位
        }
        else if (tag == QLatin1String("w:br") || tag == QLatin1String("w:cr"))
        {
            pieces << QStringLiteral("<br/>");
        }
        else if (tag == QLatin1String("w:noBreakHyphen"))
        {
            pieces << QStringLiteral("&#8209;");
        }
        else if (tag == QLatin1String("w:softHyphen"))
        {
            // 软连字符不显示，交给排版决定；这里直接忽略
        }
        else if (tag == QLatin1String("w:sym"))
        {
            bool ok = false;
            const uint code = e.attribute(QStringLiteral("w:char")).toUInt(&ok, 16);
            // 0xF000 以上是 Symbol / Wingdings 的私用区，没有对应字体时是乱码，跳过
            if (ok && code > 0 && code < 0xF000)
                pieces << esc(QString(QChar(ushort(code))));
        }
        else if (tag == QLatin1String("w:drawing") || tag == QLatin1String("w:pict"))
        {
            const QString img = imageTag(e, scale);
            if (!img.isEmpty()) pieces << img;
        }
        // w:instrText / w:fldChar / w:delText / w:object 等：不显示
    }

    if (pieces.isEmpty()) return;

    QString html = pieces.join(QString());

    // ---- 用 HTML 标签表达开关型格式（嵌套不会互相覆盖，比 CSS 更稳）----
    if (rp.sup)         html = QStringLiteral("<sup>") + html + QStringLiteral("</sup>");
    else if (rp.sub)    html = QStringLiteral("<sub>") + html + QStringLiteral("</sub>");
    if (rp.strike)      html = QStringLiteral("<s>") + html + QStringLiteral("</s>");
    if (rp.u)           html = QStringLiteral("<u>") + html + QStringLiteral("</u>");
    if (rp.i)           html = QStringLiteral("<i>") + html + QStringLiteral("</i>");
    if (rp.b)           html = QStringLiteral("<b>") + html + QStringLiteral("</b>");

    // ---- 字号 / 颜色 / 字体 / 底纹 ----
    QStringList css;
    const double sz = (rp.sizePt > 0.0 ? rp.sizePt : m_defaultSizePt) * scale;
    css << QStringLiteral("font-size:%1pt").arg(fmtNum(sz));
    if (!rp.color.isEmpty())     css << QStringLiteral("color:") + rp.color;
    if (!rp.highlight.isEmpty()) css << QStringLiteral("background-color:") + rp.highlight;
    if (!rp.family.isEmpty())    css << QStringLiteral("font-family:") + rp.family;

    out << QStringLiteral("<span style=\"") << css.join(QLatin1Char(';'))
        << QStringLiteral("\">") << html << QStringLiteral("</span>");
}

QString WordDocument::imageTag(const QDomElement &container, double scale)
{
    Q_UNUSED(scale);

    QString relId;
    double wpx = 0.0;
    double hpx = 0.0;

    const QDomElement blip = findDescendant(container, QStringLiteral("a:blip"));
    if (!blip.isNull())
    {
        relId = blip.attribute(QStringLiteral("r:embed"));
        if (relId.isEmpty()) relId = blip.attribute(QStringLiteral("r:link"));

        QDomElement extent = findDescendant(container, QStringLiteral("wp:extent"));
        if (extent.isNull()) extent = findDescendant(container, QStringLiteral("a:ext"));
        if (!extent.isNull())
        {
            wpx = extent.attribute(QStringLiteral("cx")).toDouble() / kEmuPerPx;
            hpx = extent.attribute(QStringLiteral("cy")).toDouble() / kEmuPerPx;
        }
    }
    else
    {
        // VML（Word 2007 及更早的图文框）形式：<w:pict><v:shape style="width:..;height:..">
        const QDomElement imagedata = findDescendant(container, QStringLiteral("v:imagedata"));
        if (imagedata.isNull()) return QString();
        relId = imagedata.attribute(QStringLiteral("r:id"));

        const QDomElement shape = findDescendant(container, QStringLiteral("v:shape"));
        if (!shape.isNull())
        {
            const QString style = shape.attribute(QStringLiteral("style"));
            for (const QString &part : style.split(QLatin1Char(';'), Qt::SkipEmptyParts))
            {
                const int colon = part.indexOf(QLatin1Char(':'));
                if (colon <= 0) continue;
                const QString key = part.left(colon).trimmed().toLower();
                const QString val = part.mid(colon + 1).trimmed();
                if (key == QLatin1String("width"))       wpx = parseLengthPx(val);
                else if (key == QLatin1String("height")) hpx = parseLengthPx(val);
            }
        }
    }

    if (relId.isEmpty()) return QString();

    const QString target = m_docRels.value(relId);
    if (target.isEmpty()) return QString();
    const QString part = resolvePart(m_mainPartDir, target);
    if (part.isEmpty()) return QString();       // 外链图片，不联网加载

    QString url = m_targetToUrl.value(part);
    if (url.isEmpty())
    {
        const QByteArray bytes = m_zip.entry(part);
        if (bytes.isEmpty() || bytes.size() > kMaxImageBytes) return QString();
        const QImage img = QImage::fromData(bytes);
        if (img.isNull()) return QString();      // WMF/EMF 等 Qt 不支持的格式

        url = m_urlPrefix + QString::number(++m_imageSeq);
        m_targetToUrl.insert(part, url);
        m_imageCache.insert(url, img);
    }

    // 超大图按显示宽度上限等比缩小，避免一张几万像素的图把版面撑爆
    if (wpx > kMaxImageDisplayW)
    {
        if (hpx > 0.0) hpx = hpx * kMaxImageDisplayW / wpx;
        wpx = kMaxImageDisplayW;
    }

    QString tag = QStringLiteral("<img src=\"") + url + QLatin1Char('"');
    if (wpx >= 1.0) tag += QStringLiteral(" width=\"%1\"").arg(int(wpx + 0.5));
    if (hpx >= 1.0) tag += QStringLiteral(" height=\"%1\"").arg(int(hpx + 0.5));
    tag += QLatin1Char('>');
    return tag;
}

void WordDocument::emitTable(const QDomElement &tbl, QStringList &out, double scale, int depth)
{
    if (++m_blockBudget > kMaxBlockCount) { m_truncated = true; return; }

    struct CellInfo {
        QDomElement el;
        int  colStart = 0;
        int  colSpan  = 1;
        int  vMerge   = 0;      // 0=无, 1=起始, 2=续
        int  rowSpan  = 1;
        bool covered  = false;  // 被上方的 rowSpan 覆盖，不输出
    };

    // ---- 1) 按行收集单元格，算出网格列位置 ----
    QVector<QVector<CellInfo>> rows;
    for (QDomElement tr = tbl.firstChildElement(QStringLiteral("w:tr"));
         !tr.isNull(); tr = tr.nextSiblingElement(QStringLiteral("w:tr")))
    {
        QVector<CellInfo> row;
        int col = 0;
        for (QDomElement tc = tr.firstChildElement(QStringLiteral("w:tc"));
             !tc.isNull(); tc = tc.nextSiblingElement(QStringLiteral("w:tc")))
        {
            CellInfo c;
            c.el = tc;
            c.colStart = col;

            const QDomElement tcPr = tc.firstChildElement(QStringLiteral("w:tcPr"));
            const int gs = childVal(tcPr, QStringLiteral("w:gridSpan")).toInt();
            if (gs > 0) c.colSpan = gs;

            const QDomElement vm = firstChild(tcPr, QStringLiteral("w:vMerge"));
            if (!vm.isNull())
            {
                const QString v = vm.attribute(QStringLiteral("w:val")).trimmed().toLower();
                c.vMerge = (v == QLatin1String("restart")) ? 1 : 2;   // 缺省 val 表示「续」
            }

            col += c.colSpan;
            row.append(c);
        }
        if (!row.isEmpty()) rows.append(row);
    }
    if (rows.isEmpty()) return;

    ++m_stats.tables;

    // ---- 2) 纵向合并：把「续」单元格折算成起始单元格的 rowspan ----
    QHash<int, CellInfo *> ownerOfCol;   // 列号 -> 该列当前打开的起始单元格
    for (int ri = 0; ri < rows.size(); ++ri)
    {
        QVector<CellInfo> &row = rows[ri];
        for (int ci = 0; ci < row.size(); ++ci)
        {
            CellInfo &c = row[ci];
            if (c.vMerge == 2)
            {
                CellInfo *owner = ownerOfCol.value(c.colStart, nullptr);
                if (owner != nullptr) { owner->rowSpan += 1; c.covered = true; }
            }
            else if (c.vMerge == 1)
            {
                ownerOfCol.insert(c.colStart, &c);
            }
            else
            {
                ownerOfCol.remove(c.colStart);
            }
        }
    }

    // ---- 3) 输出 ----
    const bool borders = tableHasBorders(tbl);
    out << QStringLiteral("<table border=\"%1\" cellspacing=\"0\" cellpadding=\"4\">")
               .arg(borders ? 1 : 0);

    for (const QVector<CellInfo> &row : rows)
    {
        out << QStringLiteral("<tr>");
        for (const CellInfo &c : row)
        {
            if (c.covered) continue;

            const QDomElement tcPr = c.el.firstChildElement(QStringLiteral("w:tcPr"));
            QString fill;
            const QDomElement shd = firstChild(tcPr, QStringLiteral("w:shd"));
            if (!shd.isNull())
            {
                const QString f = cssColor(shd.attribute(QStringLiteral("w:fill")));
                if (!f.isEmpty() && f != QLatin1String("#FFFFFF")) fill = f;
            }

            out << QStringLiteral("<td");
            if (c.colSpan > 1) out << QStringLiteral(" colspan=\"%1\"").arg(c.colSpan);
            if (c.rowSpan > 1) out << QStringLiteral(" rowspan=\"%1\"").arg(c.rowSpan);
            if (!fill.isEmpty())
                out << QStringLiteral(" bgcolor=\"") + fill + QStringLiteral("\"");
            out << QStringLiteral(">");

            QStringList cell;
            emitBlocks(c.el, cell, scale, depth + 1);
            if (cell.isEmpty()) out << QStringLiteral("&#160;");
            else                out << cell.join(QString());

            out << QStringLiteral("</td>");
        }
        out << QStringLiteral("</tr>");
    }
    out << QStringLiteral("</table>");
}

// ============================================================================
// ODT（OpenDocument Text）支持
// ----------------------------------------------------------------------------
// 与 docx 共用：ZipReader（容器）、m_urlPrefix / m_imageCache（图片资源）、
// m_blockBudget / m_truncated（体量限制）、render(fontScale)（缩放）。
// 差异只在 XML 形状：元素是 text:p / text:h / text:span / text:list / table:table /
// draw:frame，格式写在 style:*-properties 里、样式名经 style:name 索引，
// 样式可沿 style:parent-style-name 继承。
// ============================================================================

// "12pt" / "0.5cm" / "1.27cm" / "0.75in" / "12px" -> pt（无法识别返回 0）
double WordDocument::odtLenToPt(const QString &value)
{
    static const QRegularExpression re(
        QStringLiteral("^\\s*(-?[0-9]*\\.?[0-9]+)\\s*(pt|px|cm|mm|in|pc|em)?\\s*$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(value);
    if (!m.hasMatch()) return 0.0;

    const double n = m.captured(1).toDouble();
    const QString unit = m.captured(2).toLower();
    if (unit.isEmpty() || unit == QLatin1String("pt")) return n;
    if (unit == QLatin1String("px")) return n * 72.0 / 96.0;
    if (unit == QLatin1String("cm")) return n * 72.0 / 2.54;
    if (unit == QLatin1String("mm")) return n * 72.0 / 25.4;
    if (unit == QLatin1String("in")) return n * 72.0;
    if (unit == QLatin1String("pc")) return n * 12.0;
    if (unit == QLatin1String("em")) return n * 10.5;   // 近似：按默认字号
    return 0.0;
}

WordDocument::OdtRun WordDocument::odtMergeRun(const OdtRun &base, const OdtRun &over)
{
    OdtRun r = base;
    if (over.b) r.b = true;
    if (over.i) r.i = true;
    if (over.u) r.u = true;
    if (over.sizePt > 0.0)     r.sizePt = over.sizePt;
    if (!over.color.isEmpty()) r.color = over.color;
    if (!over.fill.isEmpty())  r.fill = over.fill;
    if (!over.family.isEmpty()) r.family = over.family;
    return r;
}

WordDocument::OdtRun WordDocument::odtRunProps(const QString &styleName) const
{
    OdtRun r;
    QString cur = styleName;
    QSet<QString> seen;
    while (!cur.isEmpty() && !seen.contains(cur))
    {
        seen.insert(cur);
        const QDomElement st = m_odtStyles.value(cur);
        if (st.isNull()) break;

        const QDomElement tp = st.firstChildElement(QStringLiteral("style:text-properties"));
        if (!tp.isNull())
        {
            const QString fw = tp.attribute(QStringLiteral("fo:font-weight"));
            if (!fw.isEmpty())
                r.b = (fw.compare(QLatin1String("bold"), Qt::CaseInsensitive) == 0
                       || fw.toInt() >= 600);

            const QString fs = tp.attribute(QStringLiteral("fo:font-style"));
            if (!fs.isEmpty())
                r.i = (fs == QLatin1String("italic") || fs == QLatin1String("oblique"));

            const QString ul = tp.attribute(QStringLiteral("style:text-underline-style"));
            if (!ul.isEmpty()) r.u = (ul != QLatin1String("none"));

            const QString sz = tp.attribute(QStringLiteral("fo:font-size"));
            if (!sz.isEmpty())
            {
                const double pt = odtLenToPt(sz);
                if (pt > 0.0) r.sizePt = pt;
            }

            const QString col = tp.attribute(QStringLiteral("fo:color"));
            if (!col.isEmpty() && col != QLatin1String("transparent")) r.color = col;

            const QString bg = tp.attribute(QStringLiteral("fo:background-color"));
            if (!bg.isEmpty() && bg != QLatin1String("transparent")) r.fill = bg;

            const QString fn = tp.attribute(QStringLiteral("style:font-name"));
            if (!fn.isEmpty() && m_odtFontFaces.contains(fn))
                r.family = m_odtFontFaces.value(fn);
        }
        cur = st.attribute(QStringLiteral("style:parent-style-name"));
    }
    return r;
}

WordDocument::OdtPar WordDocument::odtParProps(const QString &styleName) const
{
    OdtPar p;
    QString cur = styleName;
    QSet<QString> seen;
    while (!cur.isEmpty() && !seen.contains(cur))
    {
        seen.insert(cur);
        const QDomElement st = m_odtStyles.value(cur);
        if (st.isNull()) break;

        const QDomElement pp = st.firstChildElement(QStringLiteral("style:paragraph-properties"));
        if (!pp.isNull())
        {
            const QString al = pp.attribute(QStringLiteral("fo:text-align")).toLower();
            if (al == QLatin1String("center"))      p.align = HAlign::Center;
            else if (al == QLatin1String("end")
                     || al == QLatin1String("right")) p.align = HAlign::Right;
            else if (al == QLatin1String("justify")) p.align = HAlign::Justify;
            else if (al == QLatin1String("start")
                     || al == QLatin1String("left"))  p.align = HAlign::Left;

            auto takeLen = [&pp](const char *attr, double *dst) {
                const QString v = pp.attribute(QString::fromLatin1(attr));
                if (v.isEmpty()) return;
                *dst = odtLenToPt(v);
            };
            takeLen("fo:margin-left",  &p.leftPt);
            takeLen("fo:margin-right", &p.rightPt);
            takeLen("fo:text-indent",  &p.indentPt);
            takeLen("fo:margin-top",   &p.beforePt);
            takeLen("fo:margin-bottom", &p.afterPt);

            const QString bg = pp.attribute(QStringLiteral("fo:background-color"));
            if (!bg.isEmpty() && bg != QLatin1String("transparent")) p.fill = bg;
        }
        cur = st.attribute(QStringLiteral("style:parent-style-name"));
    }
    return p;
}

QString WordDocument::odtWrapRun(const OdtRun &r, double scale, const QString &html) const
{
    if (html.isEmpty()) return QString();

    QString s = html;
    if (r.u) s = QStringLiteral("<u>") + s + QStringLiteral("</u>");
    if (r.i) s = QStringLiteral("<i>") + s + QStringLiteral("</i>");
    if (r.b) s = QStringLiteral("<b>") + s + QStringLiteral("</b>");

    QStringList css;
    const double sz = (r.sizePt > 0.0 ? r.sizePt : m_defaultSizePt) * scale;
    css << QStringLiteral("font-size:%1pt").arg(fmtNum(sz));
    if (!r.color.isEmpty())  css << QStringLiteral("color:") + r.color;
    if (!r.fill.isEmpty())   css << QStringLiteral("background-color:") + r.fill;
    if (!r.family.isEmpty()) css << QStringLiteral("font-family:") + r.family;

    return QStringLiteral("<span style=\"") + css.join(QLatin1Char(';'))
           + QStringLiteral("\">") + s + QStringLiteral("</span>");
}

bool WordDocument::loadOdt(const QByteArray &data, QString *error)
{
    auto fail = [&](const QString &msg) {
        m_error = msg;
        if (error) *error = msg;
        return false;
    };

    if (!m_zip.open(data))
        return fail(QStringLiteral("无法解析该 ODT 文件：ZIP 容器读取失败（文件可能已损坏）。"));

    if (!loadXmlPart(QStringLiteral("content.xml"), &m_odtContent))
        return fail(QStringLiteral("无法解析该 ODT 文件：缺少 content.xml。"));

    // styles.xml 是可选的（只有自动样式的文档也能显示）
    if (!loadXmlPart(QStringLiteral("styles.xml"), &m_odtStyleDoc))
        m_odtStyleDoc = QDomDocument();

    const QDomElement root = m_odtContent.documentElement();
    m_odtText = findDescendant(root, QStringLiteral("office:text"));
    if (m_odtText.isNull())
        return fail(QStringLiteral("无法解析该 ODT 文件：找不到正文（office:text）。"));

    auto collectFonts = [this](const QDomElement &r) {
        const QDomElement decls = findDescendant(r, QStringLiteral("office:font-face-decls"));
        if (decls.isNull()) return;
        for (QDomElement ff = decls.firstChildElement(QStringLiteral("style:font-face"));
             !ff.isNull(); ff = ff.nextSiblingElement(QStringLiteral("style:font-face")))
        {
            const QString name = ff.attribute(QStringLiteral("style:name"));
            QString family = ff.attribute(QStringLiteral("svg:font-family")).trimmed();
            family.remove(QLatin1Char('\''));
            family.remove(QLatin1Char('"'));
            family = family.simplified();
            if (!name.isEmpty() && !family.isEmpty() && !m_odtFontFaces.contains(name))
                m_odtFontFaces.insert(name, family);
        }
    };
    collectFonts(root);

    // content.xml 里的自动样式（P1/T1…，正文直接引用它们）
    const QDomElement autoStyles = findDescendant(root, QStringLiteral("office:automatic-styles"));
    for (QDomElement st = autoStyles.firstChildElement(QStringLiteral("style:style")); !st.isNull();
         st = st.nextSiblingElement(QStringLiteral("style:style")))
    {
        const QString name = st.attribute(QStringLiteral("style:name"));
        if (!name.isEmpty()) m_odtStyles.insert(name, st);
    }

    // styles.xml 里的命名样式与列表样式
    if (!m_odtStyleDoc.isNull())
    {
        const QDomElement sroot = m_odtStyleDoc.documentElement();
        collectFonts(sroot);

        const QDomElement styles = findDescendant(sroot, QStringLiteral("office:styles"));
        if (!styles.isNull())
        {
            for (QDomElement st = styles.firstChildElement(QStringLiteral("style:style")); !st.isNull();
                 st = st.nextSiblingElement(QStringLiteral("style:style")))
            {
                const QString name = st.attribute(QStringLiteral("style:name"));
                if (!name.isEmpty() && !m_odtStyles.contains(name))
                    m_odtStyles.insert(name, st);
            }

            for (QDomElement ls = styles.firstChildElement(QStringLiteral("text:list-style")); !ls.isNull();
                 ls = ls.nextSiblingElement(QStringLiteral("text:list-style")))
            {
                const QString name = ls.attribute(QStringLiteral("style:name"));
                if (name.isEmpty()) continue;
                const bool ordered =
                    !ls.firstChildElement(QStringLiteral("text:list-level-style-number")).isNull();
                m_odtListOrdered.insert(name, ordered);
            }
        }
    }

    // 文档标题（meta.xml，可选）
    QDomDocument meta;
    if (loadXmlPart(QStringLiteral("meta.xml"), &meta))
    {
        const QDomElement t = findDescendant(meta.documentElement(), QStringLiteral("dc:title"));
        if (!t.isNull()) m_title = t.text().trimmed();
    }
    if (m_title.isEmpty()) m_title = QStringLiteral("未命名文档");

    m_isOdt = true;
    m_ok = true;
    return true;
}

void WordDocument::emitOdtBody(const QDomElement &parent, QStringList &out, double scale, int depth)
{
    if (parent.isNull() || depth > kMaxDepth) return;

    for (QDomElement e = parent.firstChildElement(); !e.isNull(); e = e.nextSiblingElement())
    {
        if (m_truncated) return;
        const QString tag = e.tagName();

        if (tag == QLatin1String("text:p") || tag == QLatin1String("text:h"))
        {
            const bool heading = (tag == QLatin1String("text:h"));
            const int lvl = e.attribute(QStringLiteral("text:outline-level")).toInt();
            emitOdtParagraph(e, out, scale, heading, lvl > 0 ? lvl : 1, 0, QString());
        }
        else if (tag == QLatin1String("text:list"))
        {
            emitOdtList(e, out, scale, 0);
        }
        else if (tag == QLatin1String("table:table"))
        {
            emitOdtTable(e, out, scale, depth);
        }
        else if (tag == QLatin1String("text:section")
                 || tag == QLatin1String("text:index-body"))
        {
            emitOdtBody(e, out, scale, depth + 1);
        }
        // 其它（目录、脚注/尾注集合、文本框、表单）忽略
    }
}

void WordDocument::emitOdtParagraph(const QDomElement &e, QStringList &out, double scale,
                                    bool heading, int headingLevel, int listDepth,
                                    const QString &bulletPrefix)
{
    if (++m_blockBudget > kMaxBlockCount) { m_truncated = true; return; }

    const QString styleName = e.attribute(QStringLiteral("text:style-name"));
    OdtRun base = odtRunProps(styleName);
    OdtPar pp   = odtParProps(styleName);

    if (heading && base.sizePt <= 0.0)
    {
        static const double kHeadSize[6] = {20.0, 16.0, 14.0, 12.0, 11.0, 10.5};
        base.sizePt = kHeadSize[qBound(1, headingLevel, 6) - 1];
        base.b = true;
    }

    QStringList runs;
    emitOdtInline(e, runs, base, scale, QString());

    ++m_stats.paragraphs;
    if (heading) ++m_stats.headings;

    QStringList css;
    switch (pp.align)
    {
    case HAlign::Left:    css << QStringLiteral("text-align:left");    break;
    case HAlign::Center:  css << QStringLiteral("text-align:center");  break;
    case HAlign::Right:   css << QStringLiteral("text-align:right");   break;
    case HAlign::Justify: css << QStringLiteral("text-align:justify"); break;
    case HAlign::Inherit: break;
    }

    double left = pp.leftPt + listDepth * kListIndentPt;
    if (!bulletPrefix.isEmpty() && pp.indentPt == 0.0) left += kListIndentPt;
    if (left > 0.0)
        css << QStringLiteral("margin-left:%1px").arg(fmtNum(left * kPtToPx));
    if (pp.rightPt > 0.0)
        css << QStringLiteral("margin-right:%1px").arg(fmtNum(pp.rightPt * kPtToPx));

    double indent = pp.indentPt;
    if (!bulletPrefix.isEmpty() && indent == 0.0) indent = -kListIndentPt;
    if (indent != 0.0)
        css << QStringLiteral("text-indent:%1px").arg(fmtNum(indent * kPtToPx));

    if (pp.beforePt > 0.0)
        css << QStringLiteral("margin-top:%1px").arg(fmtNum(pp.beforePt * kPtToPx));
    if (pp.afterPt > 0.0)
        css << QStringLiteral("margin-bottom:%1px").arg(fmtNum(pp.afterPt * kPtToPx));
    if (!pp.fill.isEmpty()) css << QStringLiteral("background-color:") + pp.fill;

    const double baseSize = (base.sizePt > 0.0 ? base.sizePt : m_defaultSizePt) * scale;
    css << QStringLiteral("font-size:%1pt").arg(fmtNum(baseSize));
    if (heading) css << QStringLiteral("font-weight:bold");

    out << QStringLiteral("<p style=\"") + css.join(QLatin1Char(';')) + QStringLiteral("\">");
    if (!bulletPrefix.isEmpty())
        out << QStringLiteral("<span>") + esc(bulletPrefix) + QStringLiteral("</span>");
    out << runs.join(QString());
    out << QStringLiteral("</p>");
}

void WordDocument::emitOdtInline(const QDomElement &parent, QStringList &out,
                                 const OdtRun &base, double scale, const QString &link)
{
    for (QDomNode n = parent.firstChild(); !n.isNull(); n = n.nextSibling())
    {
        if (n.isText())
        {
            const QString text = n.toText().data();
            if (!text.isEmpty())
            {
                m_stats.chars += int(text.size());
                out << esc(preserveSpaces(text));
            }
            continue;
        }
        if (!n.isElement()) continue;

        const QDomElement e = n.toElement();
        const QString tag = e.tagName();

        if (tag == QLatin1String("text:s"))
        {
            int count = e.attribute(QStringLiteral("text:c")).toInt();
            if (count <= 0) count = 1;
            if (count > 200) count = 200;
            for (int i = 0; i < count; ++i) out << QStringLiteral("&#160;");
        }
        else if (tag == QLatin1String("text:tab"))
        {
            out << QStringLiteral("&#160;&#160;&#160;&#160;");
        }
        else if (tag == QLatin1String("text:line-break"))
        {
            out << QStringLiteral("<br/>");
        }
        else if (tag == QLatin1String("draw:frame") || tag == QLatin1String("draw:custom-shape"))
        {
            const QString img = odtImageTag(e, scale);
            if (!img.isEmpty()) out << img;
        }
        else if (tag == QLatin1String("text:span"))
        {
            const OdtRun own = odtRunProps(e.attribute(QStringLiteral("text:style-name")));
            const OdtRun merged = odtMergeRun(base, own);

            QStringList inner;
            emitOdtInline(e, inner, merged, scale, link);
            const QString html = inner.join(QString());

            const bool hasFormat = own.b || own.i || own.u || own.sizePt > 0.0
                                   || !own.color.isEmpty() || !own.fill.isEmpty()
                                   || !own.family.isEmpty();
            out << (hasFormat ? odtWrapRun(merged, scale, html) : html);
        }
        else if (tag == QLatin1String("text:a"))
        {
            const QString href = e.attribute(QStringLiteral("xlink:href"));
            QStringList inner;
            emitOdtInline(e, inner, base, scale, href);
            const QString html = inner.join(QString());
            const QString low = href.toLower();
            if (low.startsWith(QLatin1String("http")) || low.startsWith(QLatin1String("mailto"))
                || low.startsWith(QLatin1String("ftp")))
                out << QStringLiteral("<a href=\"") + escAttr(href) + QStringLiteral("\">")
                    + html + QStringLiteral("</a>");
            else
                out << html;         // 文档内跳转/外链资源不处理
        }
        else if (tag == QLatin1String("text:note"))
        {
            // 脚注/尾注：只显示编号标记以外的正文不现实，直接跳过（与 docx 的取舍一致）
        }
        else if (tag == QLatin1String("text:bookmark")
                 || tag == QLatin1String("text:bookmark-start")
                 || tag == QLatin1String("text:bookmark-end")
                 || tag == QLatin1String("text:soft-page-break"))
        {
            // 标记类元素：不产生内容
        }
        else
        {
            // text:ruby / text:ruby-base / text:meta 等：透传子节点
            emitOdtInline(e, out, base, scale, link);
        }
    }
}

void WordDocument::emitOdtList(const QDomElement &list, QStringList &out, double scale, int depth)
{
    if (depth > kMaxDepth) return;

    const QString styleName = list.attribute(QStringLiteral("text:style-name"));
    const bool ordered = m_odtListOrdered.value(styleName, false);
    int index = 0;

    for (QDomElement item = list.firstChildElement(QStringLiteral("text:list-item"));
         !item.isNull(); item = item.nextSiblingElement(QStringLiteral("text:list-item")))
    {
        if (m_truncated) return;
        if (++m_blockBudget > kMaxBlockCount) { m_truncated = true; return; }

        ++index;
        const QString prefix = ordered ? QStringLiteral("%1. ").arg(index)
                                       : QStringLiteral("• ");
        bool emitted = false;

        for (QDomElement child = item.firstChildElement(); !child.isNull();
             child = child.nextSiblingElement())
        {
            const QString t = child.tagName();
            if (t == QLatin1String("text:p") || t == QLatin1String("text:h"))
            {
                const bool heading = (t == QLatin1String("text:h"));
                const int lvl = child.attribute(QStringLiteral("text:outline-level")).toInt();
                emitOdtParagraph(child, out, scale, heading, lvl > 0 ? lvl : 1, depth + 1,
                                 emitted ? QString() : prefix);
                emitted = true;
            }
            else if (t == QLatin1String("text:list"))
            {
                emitOdtList(child, out, scale, depth + 1);
            }
        }

        if (!emitted)
        {
            out << QStringLiteral("<p style=\"margin-left:%1px;text-indent:%2px\">")
                       .arg(fmtNum((depth + 2) * kListIndentPt * kPtToPx))
                       .arg(fmtNum(-kListIndentPt * kPtToPx))
                << QStringLiteral("<span>") + esc(prefix) + QStringLiteral("</span>")
                << QStringLiteral("</p>");
        }
    }
}

void WordDocument::emitOdtTable(const QDomElement &table, QStringList &out, double scale, int depth)
{
    if (++m_blockBudget > kMaxBlockCount) { m_truncated = true; return; }
    if (depth > kMaxDepth) return;

    ++m_stats.tables;
    out << QStringLiteral("<table border=\"1\" cellspacing=\"0\" cellpadding=\"4\" "
                          "style=\"border-collapse:collapse;font-size:%1pt\">")
               .arg(fmtNum(m_defaultSizePt * scale));

    for (QDomElement row = table.firstChildElement(QStringLiteral("table:table-row"));
         !row.isNull(); row = row.nextSiblingElement(QStringLiteral("table:table-row")))
    {
        if (m_truncated) break;
        out << QStringLiteral("<tr>");

        for (QDomElement cell = row.firstChildElement(QStringLiteral("table:table-cell"));
             !cell.isNull(); cell = cell.nextSiblingElement(QStringLiteral("table:table-cell")))
        {
            // 被合并覆盖的空单元格不输出
            if (cell.tagName() == QLatin1String("table:covered-table-cell")) continue;

            const int span = qBound(1, cell.attribute(
                QStringLiteral("table:number-columns-spanned")).toInt(), 50);

            QStringList inner;
            for (QDomElement child = cell.firstChildElement(); !child.isNull();
                 child = child.nextSiblingElement())
            {
                const QString t = child.tagName();
                if (t == QLatin1String("text:p") || t == QLatin1String("text:h"))
                {
                    const bool heading = (t == QLatin1String("text:h"));
                    const int lvl = child.attribute(QStringLiteral("text:outline-level")).toInt();
                    emitOdtParagraph(child, inner, scale, heading, lvl > 0 ? lvl : 1, 0, QString());
                }
                else if (t == QLatin1String("text:list"))
                {
                    emitOdtList(child, inner, scale, depth + 1);
                }
                else if (t == QLatin1String("table:table"))
                {
                    emitOdtTable(child, inner, scale, depth + 1);
                }
            }

            out << QStringLiteral("<td");
            if (span > 1) out << QStringLiteral(" colspan=\"%1\"").arg(span);
            out << QStringLiteral(">");
            if (inner.isEmpty()) out << QStringLiteral("&#160;");
            else                 out << inner.join(QString());
            out << QStringLiteral("</td>");
        }
        out << QStringLiteral("</tr>");
    }
    out << QStringLiteral("</table>");
}

QString WordDocument::odtImageTag(const QDomElement &frame, double scale)
{
    Q_UNUSED(scale);

    const QDomElement img = findDescendant(frame, QStringLiteral("draw:image"));
    if (img.isNull()) return QString();

    QString href = img.attribute(QStringLiteral("xlink:href")).trimmed();
    if (href.isEmpty()) return QString();
    while (href.startsWith(QLatin1String("./"))) href.remove(0, 2);
    if (href.contains(QLatin1String("://"))) return QString();   // 外链图片不联网

    QString key = href;
    QByteArray bytes = m_zip.entry(href);
    if (bytes.isEmpty())
    {
        key = QStringLiteral("Pictures/") + href;
        bytes = m_zip.entry(key);
    }
    if (bytes.isEmpty() || bytes.size() > kMaxImageBytes) return QString();

    QString url = m_targetToUrl.value(key);
    if (url.isEmpty())
    {
        const QImage im = QImage::fromData(bytes);
        if (im.isNull()) return QString();      // SVG/WMF 等 Qt 不能直接解码的格式
        url = m_urlPrefix + QString::number(++m_imageSeq);
        m_targetToUrl.insert(key, url);
        m_imageCache.insert(url, im);
    }

    double wpx = odtLenToPt(frame.attribute(QStringLiteral("svg:width"))) * kPtToPx;
    double hpx = odtLenToPt(frame.attribute(QStringLiteral("svg:height"))) * kPtToPx;
    if (wpx > kMaxImageDisplayW)
    {
        if (hpx > 0.0) hpx = hpx * kMaxImageDisplayW / wpx;
        wpx = kMaxImageDisplayW;
    }

    QString tag = QStringLiteral("<img src=\"") + url + QLatin1Char('"');
    if (wpx >= 1.0) tag += QStringLiteral(" width=\"%1\"").arg(int(wpx + 0.5));
    if (hpx >= 1.0) tag += QStringLiteral(" height=\"%1\"").arg(int(hpx + 0.5));
    tag += QLatin1Char('>');
    return tag;
}

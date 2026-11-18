#include "rtfdocument.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QtGlobal>

#include <cctype>

namespace {

// 体量上限（防御畸形/超大文件把 UI 卡死）
constexpr int kMaxParagraphs = 50000;
constexpr int kMaxChars      = 20000000;   // 2000 万字符
// 控制字参数上限：畸形文件可能给出几千位数字，累加超过这个量级就饱和处理，
// 避免 param*10 触发有符号整数溢出（UB）（P-12）
constexpr int kMaxControlParam = 1000000000;   // 1e9

// Qt 富文本的 margin-* / text-indent 只认 px（详见 worddocument.cpp 的说明）
constexpr double kPtToPx = 96.0 / 72.0;

QString esc(const QString &s)
{
    QString r;
    r.reserve(s.size() + 16);
    for (const QChar ch : s)
    {
        switch (ch.unicode())
        {
        case '&': r += QLatin1String("&amp;"); break;
        case '<': r += QLatin1String("&lt;");  break;
        case '>': r += QLatin1String("&gt;");  break;
        default:  r += ch;                     break;
        }
    }
    return r;
}

// HTML 属性值转义：字体名等来自文件的字符串要拼进 style="..."，引号必须转义（P-08）
QString escAttr(const QString &s)
{
    QString r = esc(s);
    r.replace(QLatin1Char('"'), QLatin1String("&quot;"));
    r.replace(QLatin1Char('\''), QLatin1String("&#39;"));
    return r;
}

QString fmtNum(double v)
{
    QString s = QString::number(v, 'f', 2);
    while (s.endsWith(QLatin1Char('0'))) s.chop(1);
    if (s.endsWith(QLatin1Char('.'))) s.chop(1);
    return s;
}

// 字体名只保留安全字符，避免内容里的怪字符破坏 style 属性（与 worddocument.cpp 同口径）
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

// 这些控制字标记的分组是"不显示的内容"，解析时整组跳过
bool isSkippedDestination(const QString &w)
{
    static const QSet<QString> kDests = {
        QStringLiteral("fonttbl"),  QStringLiteral("colortbl"),
        QStringLiteral("stylesheet"), QStringLiteral("info"),
        QStringLiteral("pict"),     QStringLiteral("object"),
        QStringLiteral("header"),   QStringLiteral("headerl"),
        QStringLiteral("headerr"),  QStringLiteral("headerf"),
        QStringLiteral("footer"),   QStringLiteral("footerl"),
        QStringLiteral("footerr"),  QStringLiteral("footerf"),
        QStringLiteral("footnote"), QStringLiteral("annotation"),
        /* field 组不跳过：fldinst 指令组仍跳过，fldrslt 的结果文字应显示 */
        QStringLiteral("fldinst"),
        QStringLiteral("filetbl"),  QStringLiteral("listtable"),
        QStringLiteral("listoverridetable"), QStringLiteral("rsidtbl"),
        QStringLiteral("generator"), QStringLiteral("xmlnstbl"),
        QStringLiteral("themedata"), QStringLiteral("colorschememapping"),
        QStringLiteral("datastore"), QStringLiteral("latentstyles"),
        QStringLiteral("pgptbl"),   QStringLiteral("mmathPr"),
        QStringLiteral("revtbl"),   QStringLiteral("bkmkstart"),
        QStringLiteral("bkmkend"),  QStringLiteral("xe"),
        QStringLiteral("tc"),       QStringLiteral("txe"),
    };
    return kDests.contains(w);
}

} // namespace

RtfDocument::RtfDocument() = default;

void RtfDocument::reset()
{
    m_data.clear();
    m_pos = 0;
    m_stateStack.clear();
    m_state = State();
    m_skipStack.clear();
    m_paras.clear();
    m_para = Para();
    m_pendingRuns.clear();
    m_textBuf.clear();
    m_pendingBytes.clear();
    m_fonts.clear();
    m_colors.clear();
    m_defaultSizePt = 10.5;
    m_ok = false;
    m_skipUnicodeFallback = false;
    m_ucCount = 1;
    m_fallbackRemaining = 0;
    m_ucStack.clear();
    m_chars = 0;
}

void RtfDocument::flushParagraph()
{
    flushRun();
    if (m_paras.size() >= kMaxParagraphs) return;
    // 对齐/缩进属于刚结束的段落；必须先写入再 append，否则每段都滞后一段。
    m_para.align    = m_paraAlign;
    m_para.leftPt   = m_paraLeftPt;
    m_para.indentPt = m_paraIndentPt;
    m_paras.append(m_para);
    m_para = Para();
}

void RtfDocument::flushRun()
{
    decodePendingBytes();
    if (m_textBuf.isEmpty()) return;
    Run r;
    r.b = m_state.b; r.i = m_state.i; r.u = m_state.u;
    r.strike = m_state.strike; r.sup = m_state.sup; r.sub = m_state.sub;
    r.sizePt = m_state.sizePt;
    r.color = m_state.color;
    r.family = m_state.family;
    r.text = m_textBuf;
    m_textBuf.clear();
    if (m_chars < kMaxChars)
    {
        m_chars += int(r.text.size());
        m_para.runs.append(r);
    }
}

void RtfDocument::appendText(const QString &text)
{
    if (text.isEmpty()) return;
    const int remaining = kMaxChars - m_chars - m_textBuf.size();
    if (remaining <= 0) return;
    m_textBuf += text.left(remaining);   // 单段长文本也必须受 kMaxChars 约束
}

void RtfDocument::appendByte(unsigned char byte)
{
    if (m_pendingBytes.size() >= 1024 * 1024) return;   // 防止 \'hh 洪泛
    m_pendingBytes.append(static_cast<char>(byte));
}

void RtfDocument::decodePendingBytes()
{
    if (m_pendingBytes.isEmpty()) return;
    // RTF 里的 \'hh 是"ANSI 代码页"字节；中文环境按本地编码（GBK）解码。
    const QString decoded = QString::fromLocal8Bit(m_pendingBytes);
    m_pendingBytes.clear();
    const int remaining = kMaxChars - m_chars - m_textBuf.size();
    if (remaining > 0) m_textBuf += decoded.left(remaining);
}

// ---- 字体表 / 颜色表：整组跳过渲染，但要先从中取出映射 ----
static void parseFontTable(const QByteArray &data, QHash<int, QString> *fonts)
{
    const int start = data.indexOf("{\\fonttbl");
    if (start < 0) return;
    const int end = data.indexOf("}}", start);
    const QByteArray block = data.mid(start, end < 0 ? data.size() - start : end - start + 2);

    static const QRegularExpression re(
        QStringLiteral("\\{\\\\f(\\d+)([^;]*);"),
        QRegularExpression::DotMatchesEverythingOption);
    QRegularExpressionMatchIterator it = re.globalMatch(QString::fromLatin1(block));
    while (it.hasNext())
    {
        const QRegularExpressionMatch m = it.next();
        const int idx = m.captured(1).toInt();
        QString name = m.captured(2);
        // 去掉字体表里的控制字（\froman\fcharset0 等）
        name.remove(QRegularExpression(QStringLiteral("\\\\[a-zA-Z]+-?\\d* ?")));
        name = name.trimmed();
        name.remove(QLatin1Char('{'));
        name = sanitizeFontFamily(name);        // 字体名会进 style 属性，先净化（P-08）
        if (!name.isEmpty()) fonts->insert(idx, name);
    }
}

static void parseColorTable(const QByteArray &data, QHash<int, QString> *colors)
{
    const int start = data.indexOf("{\\colortbl");
    if (start < 0) return;
    const int end = data.indexOf('}', start);
    const QByteArray block = data.mid(start, end < 0 ? data.size() - start : end - start);

    static const QRegularExpression re(
        QStringLiteral("\\\\red(\\d+)\\\\green(\\d+)\\\\blue(\\d+)"));
    int index = 0;   // RTF 颜色索引从 1 开始，0 表示 auto
    QRegularExpressionMatchIterator it = re.globalMatch(QString::fromLatin1(block));
    while (it.hasNext())
    {
        const QRegularExpressionMatch m = it.next();
        ++index;
        colors->insert(index, QStringLiteral("#%1%2%3")
            .arg(m.captured(1).toInt(), 2, 16, QLatin1Char('0'))
            .arg(m.captured(2).toInt(), 2, 16, QLatin1Char('0'))
            .arg(m.captured(3).toInt(), 2, 16, QLatin1Char('0')));
    }
}

bool RtfDocument::open(const QByteArray &data, QString *error)
{
    reset();
    auto fail = [&](const QString &msg) {
        m_ok = false;
        if (error) *error = msg;
        return false;
    };

    if (data.isEmpty())
        return fail(QStringLiteral("文件为空（0 字节），没有可显示的内容。"));
    if (!data.startsWith(QByteArrayLiteral("{\\rtf")))
        return fail(QStringLiteral("这不是 RTF 文件（缺少 {\\rtf 头）。\n\n"
                                   "请用 Word / WPS 打开后另存为 .rtf 或 .docx 再试。"));

    m_data = data;
    parseFontTable(m_data, &m_fonts);
    parseColorTable(m_data, &m_colors);

    // 正文从第一个 { 开始（字体表/颜色表会被 destination 跳过）
    parse();

    flushRun();
    if (m_para.runs.isEmpty() && !m_paras.isEmpty())
    {
        // 末尾没有 \par 的收尾段落：只在有内容时补上
    }
    else if (!m_para.runs.isEmpty())
    {
        flushParagraph();
    }

    m_ok = true;
    return true;
}

void RtfDocument::handleControlWord(const QString &word, bool hasParam, int param)
{
    const bool on = hasParam ? (param != 0) : true;

    if (word == QLatin1String("par"))   { flushParagraph(); return; }
    if (word == QLatin1String("line"))  { appendText(QStringLiteral("\n")); return; }
    if (word == QLatin1String("tab"))   { appendText(QStringLiteral("\t")); return; }
    if (word == QLatin1String("cell"))  { appendText(QStringLiteral(" | ")); return; }
    if (word == QLatin1String("row"))   { appendText(QStringLiteral("\n")); return; }

    if (word == QLatin1String("b"))      { flushRun(); m_state.b = on; return; }
    if (word == QLatin1String("i"))      { flushRun(); m_state.i = on; return; }
    if (word == QLatin1String("ul"))     { flushRun(); m_state.u = on; return; }
    if (word == QLatin1String("ulnone")) { flushRun(); m_state.u = false; return; }
    if (word == QLatin1String("strike") || word == QLatin1String("striked"))
    { flushRun(); m_state.strike = on; return; }
    if (word == QLatin1String("super"))  { flushRun(); m_state.sup = on; m_state.sub = false; return; }
    if (word == QLatin1String("sub"))    { flushRun(); m_state.sub = on; m_state.sup = false; return; }
    if (word == QLatin1String("nosupersub"))
    { flushRun(); m_state.sup = false; m_state.sub = false; return; }

    if (word == QLatin1String("fs") && hasParam)
    { flushRun(); m_state.sizePt = qMax(1.0, param / 2.0); return; }
    if (word == QLatin1String("cf") && hasParam)
    { flushRun(); m_state.color = param > 0 ? m_colors.value(param) : QString(); return; }
    if (word == QLatin1String("f") && hasParam)
    { flushRun(); m_state.family = m_fonts.value(param); return; }

    if (word == QLatin1String("plain"))
    {
        flushRun();
        m_state = State();
        return;
    }
    if (word == QLatin1String("pard"))
    {
        m_paraAlign = 0;
        m_paraLeftPt = 0.0;
        m_paraIndentPt = 0.0;
        return;
    }
    if (word == QLatin1String("ql")) { m_paraAlign = 0; return; }
    if (word == QLatin1String("qc")) { m_paraAlign = 1; return; }
    if (word == QLatin1String("qr")) { m_paraAlign = 2; return; }
    if (word == QLatin1String("qj")) { m_paraAlign = 3; return; }
    if (word == QLatin1String("li") && hasParam) { m_paraLeftPt = param / 20.0; return; }
    if (word == QLatin1String("fi") && hasParam) { m_paraIndentPt = param / 20.0; return; }

    if (word == QLatin1String("u") && hasParam)
    {
        decodePendingBytes();
        int code = param;
        if (code < 0) code += 65536;
        if (code > 0 && code <= 0x10FFFF)
            m_textBuf += QString(QChar::fromUcs4(uint(code)));   // 代理对由 QString 处理
        m_fallbackRemaining = m_ucCount;
        m_skipUnicodeFallback = (m_fallbackRemaining > 0);
        return;
    }
    if (word == QLatin1String("uc") && hasParam)
    {
        // \ucN：\uN 之后要跳过的回退单元数（N=0/1/2...）
        m_ucCount = qBound(0, param, 10);
        return;
    }

    // 其余控制字：忽略（含 \fcharsetN、\deffN、\deflang 等文档级设置）
}

void RtfDocument::parse()
{
    const int n = m_data.size();
    int i = 0;

    m_stateStack.append(State());
    m_skipStack.append(false);

    while (i < n && m_chars < kMaxChars)
    {
        const char c = m_data.at(i);

        if (c == '{')
        {
            flushRun();
            if (m_stateStack.size() >= 4096)
            {
                // 嵌套上限：RTF 不需要几千层分组；继续会 OOM。已解析内容保留。
                m_chars = kMaxChars;
                break;
            }
            m_stateStack.append(m_state);
            m_skipStack.append(m_skipStack.last());
            m_ucStack.append(m_ucCount);
            ++i;
            continue;
        }
        if (c == '}')
        {
            flushRun();
            decodePendingBytes();
            if (m_stateStack.size() > 1) { m_stateStack.removeLast(); m_state = m_stateStack.last(); }
            if (m_skipStack.size() > 1)  m_skipStack.removeLast();
            if (m_ucStack.size() > 1)    m_ucCount = m_ucStack.last(), m_ucStack.removeLast();
            ++i;
            continue;
        }

        const bool bSkip = m_skipStack.last();
        if (c != '\\')
        {
            if (bSkip)
            {
                ++i;
                continue;
            }
            if (m_skipUnicodeFallback && m_fallbackRemaining > 0)
            {
                if (--m_fallbackRemaining == 0) m_skipUnicodeFallback = false;
                ++i;
                continue;
            }
            if (c == '\r' || c == '\n')
            {
                ++i;
                continue;
            }
            const unsigned char uc = static_cast<unsigned char>(c);
            if (uc < 0x80)
            {
                decodePendingBytes();
                appendText(QString(QChar(uc)));
            }
            else
            {
                appendByte(uc);         // 未转义的高字节：按本地编码解码
            }
            ++i;
            continue;
        }

        // ---- 控制序列 ----
        ++i;
        if (i >= n) break;
        const char c2 = m_data.at(i);

        if (c2 == '\'')
        {
            if (i + 2 < n)
            {
                auto hexVal = [](char h) -> int {
                    if (h >= '0' && h <= '9') return h - '0';
                    if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                    if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                    return -1;
                };
                const int hi = hexVal(m_data.at(i + 1));
                const int lo = hexVal(m_data.at(i + 2));
                if (hi >= 0 && lo >= 0)
                {
                    // \uN 后面紧跟的 \'hh 是 ANSI 回退，不属于正文：
                    // 命中标志时整组跳过（只清标志，不写回退字节）（P-07）
                    if (m_skipUnicodeFallback && m_fallbackRemaining > 0)
                    {
                        if (--m_fallbackRemaining == 0) m_skipUnicodeFallback = false;
                        i += 3;
                        continue;
                    }
                    if (bSkip) { /* 跳过组内字节 */ }
                    else appendByte(static_cast<unsigned char>(hi * 16 + lo));
                    i += 3;
                    m_skipUnicodeFallback = false;
                    continue;
                }
            }
            i += 1;
            continue;
        }
        if (c2 == '\\' || c2 == '{' || c2 == '}')
        {
            if (!bSkip)
            {
                decodePendingBytes();
                appendText(QString(QChar(c2)));
            }
            ++i;
            m_skipUnicodeFallback = false;
            continue;
        }
        if (c2 == '*')
        {
            m_skipStack.last() = true;   // {\* ...} 是扩展目的地，不渲染
            ++i;
            continue;
        }
        if (c2 == '\r' || c2 == '\n')
        {
            ++i;                          // 控制符号换行：等价于 \par
            flushParagraph();
            continue;
        }

        if (!std::isalpha(static_cast<unsigned char>(c2)))
        {
            ++i;                          // 未知控制符号：忽略
            continue;
        }

        // 控制字：字母序列 + 可选参数 + 可选分隔空格
        const int wordStart = i;
        while (i < n && std::isalpha(static_cast<unsigned char>(m_data.at(i)))) ++i;
        const QString word = QString::fromLatin1(m_data.mid(wordStart, i - wordStart));

        bool hasParam = false;
        int param = 0;
        bool neg = false;
        if (i < n && m_data.at(i) == '-') { neg = true; ++i; }
        while (i < n && std::isdigit(static_cast<unsigned char>(m_data.at(i))))
        {
            const int digit = m_data.at(i) - '0';
            // 累加前先检查上界：畸形超长数字饱和在 kMaxControlParam，
            // 后续数字照常跳过（解析位置不变），避免 param*10 有符号溢出 UB（P-12）
            if (param > (kMaxControlParam - digit) / 10)
                param = kMaxControlParam;
            else
                param = param * 10 + digit;
            hasParam = true;
            ++i;
        }
        if (neg) param = -param;
        if (i < n && m_data.at(i) == ' ') ++i;

        if (bSkip) continue;

        if (isSkippedDestination(word))
        {
            m_skipStack.last() = true;
            continue;
        }

        m_skipUnicodeFallback = false;
        handleControlWord(word, hasParam, param);
    }
}

QString RtfDocument::render(double fontScale) const
{
    QStringList out;
    for (const Para &p : m_paras)
    {
        QStringList css;
        switch (p.align)
        {
        case 1: css << QStringLiteral("text-align:center");  break;
        case 2: css << QStringLiteral("text-align:right");   break;
        case 3: css << QStringLiteral("text-align:justify"); break;
        default: break;
        }
        if (p.leftPt > 0.0)
            css << QStringLiteral("margin-left:%1px").arg(fmtNum(p.leftPt * kPtToPx));
        if (p.indentPt != 0.0)
            css << QStringLiteral("text-indent:%1px").arg(fmtNum(p.indentPt * kPtToPx));
        css << QStringLiteral("font-size:%1pt").arg(fmtNum(m_defaultSizePt * fontScale));

        QStringList runs;
        for (const Run &r : p.runs)
        {
            if (r.text.isEmpty()) continue;

            QString s = esc(r.text);
            s.replace(QLatin1String("\n"), QLatin1String("<br/>"));
            s.replace(QLatin1String("\t"), QStringLiteral("&#160;&#160;&#160;&#160;"));

            if (r.sup)    s = QStringLiteral("<sup>") + s + QStringLiteral("</sup>");
            else if (r.sub) s = QStringLiteral("<sub>") + s + QStringLiteral("</sub>");
            if (r.strike) s = QStringLiteral("<s>") + s + QStringLiteral("</s>");
            if (r.u)      s = QStringLiteral("<u>") + s + QStringLiteral("</u>");
            if (r.i)      s = QStringLiteral("<i>") + s + QStringLiteral("</i>");
            if (r.b)      s = QStringLiteral("<b>") + s + QStringLiteral("</b>");

            QStringList css2;
            const double sz = (r.sizePt > 0.0 ? r.sizePt : m_defaultSizePt) * fontScale;
            css2 << QStringLiteral("font-size:%1pt").arg(fmtNum(sz));
            if (!r.color.isEmpty())  css2 << QStringLiteral("color:") + r.color;
            // 字体名来自文件：先净化，拼进 style 时再按 HTML 属性值转义（P-08）
            const QString family = sanitizeFontFamily(r.family);
            if (!family.isEmpty())   css2 << QStringLiteral("font-family:") + family;

            runs << QStringLiteral("<span style=\"") + escAttr(css2.join(QLatin1Char(';')))
                    + QStringLiteral("\">") + s + QStringLiteral("</span>");
        }

        if (runs.isEmpty())
            runs << QStringLiteral("&#160;");

        out << QStringLiteral("<p style=\"") + escAttr(css.join(QLatin1Char(';')))
               + QStringLiteral("\">") + runs.join(QString()) + QStringLiteral("</p>");
    }
    return out.join(QString());
}

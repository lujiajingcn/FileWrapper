#include "sheetdocument.h"

#include <QDomDocument>
#include <QDomElement>
#include <QHash>
#include <QStringList>
#include <QRegularExpression>

namespace {

// 体量上限：只读预览，不需要渲染整张百万行表
constexpr int kMaxSheets  = 20;
constexpr int kMaxRows    = 500;
constexpr int kMaxCols    = 120;
constexpr int kMaxCells   = 30000;
constexpr int kMaxCellLen = 400;
constexpr int kMaxXlsxCols = 16384;   // xlsx 列号上限（XFD，1 基），超过视为畸形引用

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

QString fmtNum(double v)
{
    QString s = QString::number(v, 'f', 2);
    while (s.endsWith(QLatin1Char('0'))) s.chop(1);
    if (s.endsWith(QLatin1Char('.'))) s.chop(1);
    return s;
}

// "A1" / "AB12" -> (col, row)（0 基）
bool parseCellRef(const QString &ref, int *col, int *row)
{
    int i = 0;
    int c = 0;
    while (i < ref.size() && ref.at(i).isLetter())
    {
        const int digit = ref.at(i).toUpper().unicode() - 'A' + 1;
        if (digit < 1 || digit > 26) return false;           // isLetter() 也接受非 ASCII 字母
        // 累加前先判上界：畸形超长列字母（AAAA...）会在 c * 26 + digit 处 int 溢出（UB）
        if (c > (kMaxXlsxCols - digit) / 26) return false;
        c = c * 26 + digit;
        ++i;
    }
    if (i == 0 || i >= ref.size()) return false;
    const int r = ref.mid(i).toInt();
    if (r <= 0) return false;
    *col = c - 1;
    *row = r - 1;
    return true;
}

// xlsx 的 rels 只解析一次：<Relationship Id="rId1" Target="worksheets/sheet1.xml"/> -> Id -> Target。
// 原实现每个 <sheet> 都重新 setContent 解析整份 rels，畸形工作簿可借此拖死 UI。
QHash<QString, QString> parseRels(const QByteArray &relsXml)
{
    QHash<QString, QString> targets;
    QDomDocument doc;
    if (!doc.setContent(relsXml)) return targets;
    for (QDomElement e = doc.documentElement().firstChildElement(QStringLiteral("Relationship"));
         !e.isNull(); e = e.nextSiblingElement(QStringLiteral("Relationship")))
    {
        targets.insert(e.attribute(QStringLiteral("Id")), e.attribute(QStringLiteral("Target")));
    }
    return targets;
}

QString firstTextOf(const QDomElement &el, const QString &tag)
{
    for (QDomElement c = el.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
    {
        if (c.tagName() == tag) return c.text();
    }
    return QString();
}

} // namespace

void SheetDocument::reset()
{
    m_zip = ZipReader();
    m_sheets.clear();
    m_kind.clear();
    m_totalRows = 0;
    m_maxCols = 0;
    m_truncated = false;
    m_ok = false;
}

void SheetDocument::setCell(int sheetIndex, int row, int col, const QString &text)
{
    if (sheetIndex < 0 || sheetIndex >= m_sheets.size()) return;
    if (row < 0 || row >= kMaxRows || col < 0 || col >= kMaxCols) { m_truncated = true; return; }
    Sheet &s = m_sheets[sheetIndex];
    if (s.cells.size() >= kMaxCells) { m_truncated = true; return; }

    QString t = text;
    if (t.size() > kMaxCellLen) { t.truncate(kMaxCellLen); t += QStringLiteral("…"); m_truncated = true; }

    Cell c;
    c.text = t;
    s.cells[row].insert(col, c);
    s.rows = qMax(s.rows, row + 1);
    s.cols = qMax(s.cols, col + 1);
}

void SheetDocument::applyMerge(int sheetIndex, int row, int col, int rowSpan, int colSpan)
{
    if (sheetIndex < 0 || sheetIndex >= m_sheets.size()) return;
    Sheet &s = m_sheets[sheetIndex];
    auto itRow = s.cells.find(row);
    if (itRow == s.cells.end()) return;
    auto itCell = itRow->find(col);
    if (itCell == itRow->end()) return;
    itCell->rowSpan = qMax(1, rowSpan);
    itCell->colSpan = qMax(1, colSpan);

    // 被合并覆盖的格子标记为 covered，渲染时跳过（保证列位置正确）
    for (int r = row; r < row + itCell->rowSpan && r < kMaxRows; ++r)
    {
        for (int c = col; c < col + itCell->colSpan && c < kMaxCols; ++c)
        {
            if (r == row && c == col) continue;
            s.cells[r][c].covered = true;
        }
    }
}

bool SheetDocument::open(const QByteArray &data, QString *error)
{
    reset();
    auto fail = [&](const QString &msg) {
        m_ok = false;
        if (error) *error = msg;
        return false;
    };

    if (data.isEmpty())
        return fail(QStringLiteral("文件为空（0 字节），没有可显示的内容。"));
    if (!data.startsWith(QByteArrayLiteral("PK\x03\x04")))
        return fail(QStringLiteral("无法解析：这不是 xlsx/ods（ZIP 容器读取失败）。"));
    if (!m_zip.open(data))
        return fail(QStringLiteral("无法解析：ZIP 容器读取失败（文件可能已损坏）。"));

    if (m_zip.entry(QStringLiteral("mimetype")).contains("opendocument.spreadsheet"))
        return parseOds(error);

    if (m_zip.contains(QStringLiteral("xl/workbook.xml")))
        return parseXlsx(error);

    return fail(QStringLiteral("无法识别该表格文件：既不是 .xlsx（xl/workbook.xml）"
                               "也不是 .ods（opendocument.spreadsheet）。"));
}

bool SheetDocument::parseXlsx(QString *error)
{
    m_kind = QStringLiteral("xlsx");
    auto fail = [&](const QString &msg) {
        m_ok = false;
        if (error) *error = msg;
        return false;
    };

    QDomDocument wb;
    if (!wb.setContent(m_zip.entry(QStringLiteral("xl/workbook.xml"))))
        return fail(QStringLiteral("无法解析 xl/workbook.xml。"));

    const QByteArray wbRels = m_zip.entry(QStringLiteral("xl/_rels/workbook.xml.rels"));
    const QHash<QString, QString> wbRelTargets = parseRels(wbRels);

    // 共享字符串表
    QStringList sharedStrings;
    {
        QDomDocument ss;
        if (ss.setContent(m_zip.entry(QStringLiteral("xl/sharedStrings.xml"))))
        {
            for (QDomElement si = ss.documentElement().firstChildElement(QStringLiteral("si"));
                 !si.isNull(); si = si.nextSiblingElement(QStringLiteral("si")))
            {
                QString text;
                // <si><t>..</t></si> 或 <si><r><t>..</t></r>...</si>
                for (QDomElement t = si.firstChildElement(); !t.isNull(); t = t.nextSiblingElement())
                {
                    if (t.tagName() == QLatin1String("t"))
                        text += t.text();
                    else if (t.tagName() == QLatin1String("r"))
                        text += firstTextOf(t, QStringLiteral("t"));
                }
                sharedStrings.append(text);
            }
        }
    }

    // 工作表列表（按 workbook 里的顺序）；先按 kMaxSheets 截断再查表：
    // 畸形工作簿的 <sheet> 数量不受限，逐个解析整份 rels 会拖死 UI
    QVector<QPair<QString, QString>> sheetList;   // (name, target)
    bool sheetListCapped = false;
    for (QDomElement s = wb.documentElement().firstChildElement("sheets");
         !s.isNull() && !sheetListCapped; s = s.nextSiblingElement("sheets"))
    {
        for (QDomElement e = s.firstChildElement(QStringLiteral("sheet")); !e.isNull();
             e = e.nextSiblingElement(QStringLiteral("sheet")))
        {
            if (sheetList.size() >= kMaxSheets) { sheetListCapped = true; break; }
            const QString name = e.attribute(QStringLiteral("name"));
            const QString rid  = e.attribute(QStringLiteral("r:id"));
            QString target = wbRelTargets.value(rid);
            while (target.startsWith(QLatin1Char('/'))) target.remove(0, 1);
            if (!target.isEmpty() && !target.startsWith(QLatin1String("xl/")))
                target = QStringLiteral("xl/") + target;
            sheetList.append(qMakePair(name, target));
        }
    }
    if (sheetListCapped) m_truncated = true;
    if (sheetList.isEmpty())
        return fail(QStringLiteral("工作簿里没有任何工作表。"));

    for (int si = 0; si < sheetList.size() && si < kMaxSheets; ++si)
    {
        const QString &target = sheetList.at(si).second;
        QDomDocument sheetDoc;
        if (!sheetDoc.setContent(m_zip.entry(target))) continue;

        Sheet sheet;
        sheet.name = sheetList.at(si).first;
        m_sheets.append(sheet);

        const int idx = m_sheets.size() - 1;
        for (QDomElement rowEl = sheetDoc.documentElement().firstChildElement("sheetData");
             !rowEl.isNull(); rowEl = rowEl.nextSiblingElement("sheetData"))
        {
            for (QDomElement row = rowEl.firstChildElement(QStringLiteral("row")); !row.isNull();
                 row = row.nextSiblingElement(QStringLiteral("row")))
            {
                int rowIdx = row.attribute(QStringLiteral("r")).toInt() - 1;
                if (rowIdx < 0) rowIdx = 0;
                if (rowIdx >= kMaxRows) { m_truncated = true; break; }

                for (QDomElement c = row.firstChildElement(QStringLiteral("c")); !c.isNull();
                     c = c.nextSiblingElement(QStringLiteral("c")))
                {
                    int colIdx = 0;
                    if (!parseCellRef(c.attribute(QStringLiteral("r")), &colIdx, &rowIdx))
                        continue;

                    const QString type = c.attribute(QStringLiteral("t"));
                    QString text;
                    if (type == QLatin1String("s"))
                    {
                        const int n = c.firstChildElement(QStringLiteral("v")).text().toInt();
                        if (n >= 0 && n < sharedStrings.size()) text = sharedStrings.at(n);
                    }
                    else if (type == QLatin1String("inlineStr"))
                    {
                        const QDomElement is = c.firstChildElement(QStringLiteral("is"));
                        text = is.isNull() ? QString() : is.text();
                    }
                    else
                    {
                        text = c.firstChildElement(QStringLiteral("v")).text();
                    }
                    if (!text.isEmpty()) setCell(idx, rowIdx, colIdx, text);
                }
            }
        }

        // 合并单元格
        for (QDomElement mc = sheetDoc.documentElement().firstChildElement("mergeCells");
             !mc.isNull(); mc = mc.nextSiblingElement("mergeCells"))
        {
            for (QDomElement m = mc.firstChildElement(QStringLiteral("mergeCell")); !m.isNull();
                 m = m.nextSiblingElement(QStringLiteral("mergeCell")))
            {
                const QString ref = m.attribute(QStringLiteral("ref"));
                const QStringList parts = ref.split(QLatin1Char(':'));
                int c1 = 0, r1 = 0, c2 = 0, r2 = 0;
                if (parts.size() != 2) continue;
                if (!parseCellRef(parts.at(0), &c1, &r1)) continue;
                if (!parseCellRef(parts.at(1), &c2, &r2)) continue;
                if (c2 < c1 || r2 < r1) continue;
                applyMerge(idx, r1, c1, r2 - r1 + 1, c2 - c1 + 1);
            }
        }
    }

    m_ok = !m_sheets.isEmpty();
    if (!m_ok) return fail(QStringLiteral("没有可显示的工作表内容。"));
    return true;
}

bool SheetDocument::parseOds(QString *error)
{
    m_kind = QStringLiteral("ods");
    auto fail = [&](const QString &msg) {
        m_ok = false;
        if (error) *error = msg;
        return false;
    };

    QDomDocument doc;
    if (!doc.setContent(m_zip.entry(QStringLiteral("content.xml"))))
        return fail(QStringLiteral("无法解析 ODS 的 content.xml。"));

    QDomElement spreadsheet;
    std::function<void(const QDomElement &)> findSpreadsheet = [&](const QDomElement &el) {
        if (!spreadsheet.isNull()) return;
        if (el.tagName() == QLatin1String("office:spreadsheet")) { spreadsheet = el; return; }
        for (QDomElement c = el.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
            findSpreadsheet(c);
    };
    findSpreadsheet(doc.documentElement());
    if (spreadsheet.isNull())
        return fail(QStringLiteral("ODS 里找不到 office:spreadsheet。"));

    for (QDomElement table = spreadsheet.firstChildElement(QStringLiteral("table:table"));
         !table.isNull(); table = table.nextSiblingElement(QStringLiteral("table:table")))
    {
        if (m_sheets.size() >= kMaxSheets) { m_truncated = true; break; }

        Sheet sheet;
        sheet.name = table.attribute(QStringLiteral("table:name"));
        if (sheet.name.isEmpty()) sheet.name = QStringLiteral("Sheet%1").arg(m_sheets.size() + 1);
        m_sheets.append(sheet);
        const int idx = m_sheets.size() - 1;

        int rowIdx = 0;
        for (QDomElement row = table.firstChildElement(QStringLiteral("table:table-row"));
             !row.isNull() && rowIdx < kMaxRows;
             row = row.nextSiblingElement(QStringLiteral("table:table-row")))
        {
            const int rowRepeat = qBound(1, row.attribute(QStringLiteral("table:number-rows-repeated")).toInt(), 40);

            int colIdx = 0;
            for (QDomElement cell = row.firstChildElement(); !cell.isNull();
                 cell = cell.nextSiblingElement())
            {
                const QString tag = cell.tagName();
                if (tag != QLatin1String("table:table-cell")
                    && tag != QLatin1String("table:covered-table-cell"))
                    continue;

                int colRepeat = qBound(1, cell.attribute(
                    QStringLiteral("table:number-columns-repeated")).toInt(), 40);

                if (tag == QLatin1String("table:table-cell") && colIdx < kMaxCols)
                {
                    QString text;
                    for (QDomElement p = cell.firstChildElement(); !p.isNull();
                         p = p.nextSiblingElement())
                    {
                        if (p.tagName() == QLatin1String("text:p")
                            || p.tagName() == QLatin1String("text:h"))
                        {
                            if (!text.isEmpty()) text += QLatin1Char('\n');
                            text += p.text();
                        }
                    }
                    if (!text.isEmpty())
                    {
                        for (int rep = 0; rep < rowRepeat && rowIdx + rep < kMaxRows; ++rep)
                            setCell(idx, rowIdx + rep, colIdx, text);
                    }
                }
                colIdx += colRepeat;
                if (colIdx >= kMaxCols) { m_truncated = true; break; }
            }
            rowIdx += rowRepeat;
        }
    }

    m_ok = !m_sheets.isEmpty();
    if (!m_ok) return fail(QStringLiteral("没有可显示的工作表内容。"));
    return true;
}

QString SheetDocument::render(double fontScale) const
{
    QStringList out;
    const double fontPt = qBound(6.0, m_sheets.isEmpty() ? 10.0 : 10.0, 24.0) * fontScale;

    for (const Sheet &s : m_sheets)
    {
        out << QStringLiteral("<h3 style=\"font-size:%1pt\">%2</h3>")
                   .arg(fmtNum(fontPt * 1.2), esc(s.name));

        out << QStringLiteral("<table border=\"1\" cellspacing=\"0\" cellpadding=\"4\" "
                              "style=\"border-collapse:collapse;font-size:%1pt\">")
                   .arg(fmtNum(fontPt));

        for (int r = 0; r < s.rows; ++r)
        {
            const auto rowIt = s.cells.find(r);
            if (rowIt == s.cells.end()) continue;

            out << QStringLiteral("<tr>");
            for (int c = 0; c < s.cols; ++c)
            {
                const auto cellIt = rowIt->find(c);
                if (cellIt == rowIt->end())
                {
                    out << QStringLiteral("<td>&#160;</td>");
                    continue;
                }
                if (cellIt->covered) continue;

                out << QStringLiteral("<td");
                if (cellIt->rowSpan > 1) out << QStringLiteral(" rowspan=\"%1\"").arg(cellIt->rowSpan);
                if (cellIt->colSpan > 1) out << QStringLiteral(" colspan=\"%1\"").arg(cellIt->colSpan);
                out << QStringLiteral(">");

                QString text = esc(cellIt->text);
                text.replace(QLatin1String("\n"), QLatin1String("<br/>"));
                out << (text.isEmpty() ? QStringLiteral("&#160;") : text);
                out << QStringLiteral("</td>");
            }
            out << QStringLiteral("</tr>");
        }
        out << QStringLiteral("</table>");
        if (m_truncated)
            out << QStringLiteral("<p><i>（表格过大，仅显示前一部分内容）</i></p>");
    }
    return out.join(QString());
}

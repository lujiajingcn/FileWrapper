#ifndef SHEETDOCUMENT_H
#define SHEETDOCUMENT_H

#include <QByteArray>
#include <QHash>
#include <QMap>
#include <QString>
#include <QVector>

#include "zipreader.h"

// ============================================================================
// SheetDocument：把 .xlsx（OOXML SpreadsheetML）与 .ods（OpenDocument Spreadsheet）
// 的**第一层数据**转成只读 HTML 表格，交给 QTextBrowser 显示。
//
// xlsx：
//   xl/workbook.xml                  -> 工作表列表（name + r:id）
//   xl/_rels/workbook.xml.rels       -> r:id -> worksheets/sheetN.xml
//   xl/sharedStrings.xml             -> 共享字符串表（t="s" 的单元格是索引）
//   xl/worksheets/sheetN.xml         -> <row><c r="A1" t="s"><v>..</v></c>；<mergeCells>
// ods：
//   content.xml                      -> office:spreadsheet/table:table/table:table-row/table:table-cell
//
// 只做"看得到内容"：不计算公式（用缓存值）、不处理样式/条件格式/图表/批注。
// 行/列/单元格都有上限，防止超大表把 UI 卡死。
// ============================================================================
class SheetDocument
{
public:
    bool open(const QByteArray &data, QString *error = nullptr);
    void reset();
    bool isOpen() const { return m_ok; }

    // 只读表格 HTML。fontScale 供缩放使用（1.0 = 原始字号）。
    QString render(double fontScale = 1.0) const;

    QString kind() const { return m_kind; }        // "xlsx" / "ods"
    int sheetCount() const { return m_sheets.size(); }
    int rowCount() const { return m_totalRows; }
    int colCount() const { return m_maxCols; }
    bool truncated() const { return m_truncated; }
    // P2-5：数据本身有问题（共享字符串索引非法等），与「表格过大被截断」区分开
    bool hasParseProblem() const { return m_parseProblem; }
    // P1-12/13/16、P2-6：截断或异常的中文原因（空 = 未触发）
    QString truncatedReason() const { return m_truncatedReason; }

private:
    struct Cell {
        QString text;
        int     rowSpan = 1;
        int     colSpan = 1;
        bool    covered = false;
    };
    struct Sheet {
        QString name;
        int rows = 0;
        int cols = 0;
        QMap<int, QMap<int, Cell>> cells;   // row -> col -> cell
        int cellCount = 0;                  // 已插入的格子总数（含 merge 覆盖格）
    };

    bool parseXlsx(QString *error);
    bool parseOds(QString *error);
    void setCell(int sheetIndex, int row, int col, const QString &text);
    void applyMerge(int sheetIndex, int row, int col, int rowSpan, int colSpan);

    // 截断 / 解析异常记账（原因只记第一次，避免被后续更细碎的原因覆盖）
    void markTruncated(const QString &reason);
    void markParseProblem(const QString &reason);

    ZipReader       m_zip;
    QVector<Sheet>  m_sheets;
    QString         m_kind;
    int             m_totalRows = 0;
    int             m_maxCols   = 0;
    bool            m_truncated = false;
    bool            m_parseProblem = false;
    QString         m_truncatedReason;
    bool            m_ok = false;
};

#endif // SHEETDOCUMENT_H

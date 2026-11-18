#ifndef RTFDOCUMENT_H
#define RTFDOCUMENT_H

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVector>

// ============================================================================
// RtfDocument：把 .rtf 的**常用子集**转换成 QTextBrowser 能渲染的 HTML。
//
// RTF 是"控制字 + 嵌套分组"的纯文本格式，不依赖 ZIP/XML，因此与 docx/odt 的
// 解析路径完全不同；这里只实现文档类 RTF 里最常见的部分：
//   * 段落：\par、行内 \line、\tab，对齐 \ql/\qc/\qr/\qj，\li/\fi 缩进
//   * 字符：\b \i \ul \strike \super \sub、\fs（半磅）、\cf（颜色表）、\f（字体表）
//   * 编码：\'hh（按本地编码解码）与 \uN（Unicode，跳过回退字符）
//   * 跳过：\fonttbl/\colortbl/\stylesheet/\info/\pict/页眉页脚等不显示的分组
//   * 表格：\cell → " | "、\row → 换行（只做粗糙的文本化，不做真正表格）
//
// 已知不支持（静默忽略）：图片、真正表格、脚注、域、修订、文本框、嵌入对象。
// ============================================================================
class RtfDocument
{
public:
    struct Run {
        bool    b       = false;
        bool    i       = false;
        bool    u       = false;
        bool    strike  = false;
        bool    sup     = false;
        bool    sub     = false;
        double  sizePt  = 0.0;      // 0 = 用文档默认字号
        QString color;              // #RRGGBB
        QString family;             // 字体名
        QString text;
    };

    struct Para {
        QVector<Run> runs;
        int     align    = 0;       // 0=左 1=居中 2=右 3=两端
        double  leftPt   = 0.0;
        double  indentPt = 0.0;
    };

    RtfDocument();

    bool open(const QByteArray &data, QString *error = nullptr);
    void reset();
    bool isOpen() const { return m_ok; }

    // 生成 HTML 片段。fontScale=1.0 为原始字号；>1 放大，<1 缩小。
    QString render(double fontScale) const;

    const QVector<Para> &paragraphs() const { return m_paras; }
    double defaultSizePt() const { return m_defaultSizePt; }
    int    charCount() const { return m_chars; }

private:
    void parse();
    void flushParagraph();
    void flushRun();
    void appendText(const QString &text);
    void appendByte(unsigned char byte);
    void decodePendingBytes();
    void handleControlWord(const QString &word, bool hasParam, int param);

    struct State {
        bool    b = false, i = false, u = false, strike = false, sup = false, sub = false;
        double  sizePt = 0.0;
        QString color;
        QString family;
    };

    QByteArray m_data;
    int        m_pos = 0;
    QVector<State> m_stateStack;
    State          m_state;
    QVector<bool>  m_skipStack;
    QVector<int>   m_ucStack;
    QVector<Para>  m_paras;
    Para           m_para;
    QVector<Run>   m_pendingRuns;   // 当前段已完成的 run
    QString        m_textBuf;       // 当前 run 的文本
    QString        m_currentRunStyleKey;   // 文本缓冲对应的格式（用于切分 run）
    QByteArray     m_pendingBytes;  // 连续的 \'hh 字节（需要一起按本地编码解码）
    int            m_paraAlign    = 0;    // 正在构建的段落属性（\pard 重置）
    double         m_paraLeftPt   = 0.0;
    double         m_paraIndentPt = 0.0;
    QHash<int, QString> m_fonts;    // \fN -> 字体名
    QHash<int, QString> m_colors;   // \cfN -> #RRGGBB
    double         m_defaultSizePt = 10.5;
    bool           m_ok = false;
    bool           m_skipUnicodeFallback = false;
    int            m_ucCount = 1;          // \ucN 的 N（默认为 1）
    int            m_fallbackRemaining = 0;
    int            m_chars = 0;
};

#endif // RTFDOCUMENT_H

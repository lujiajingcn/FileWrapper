#include "builtinviewer.h"

#include <QWidget>
#include <QLabel>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QFont>
#include <QByteArray>
#include <QStringDecoder>

namespace {

// P2-1/P2-2：严格解码文本。
// 旧实现用 QString::fromUtf8() 后检查 U+FFFD：既会把「合法包含 U+FFFD 的文本」
// 误判成坏编码，也会把 1 MiB 截断处的半个多字节序列当成整个文件非法而回退本地编码。
// 这里用可报告错误的 QStringDecoder：
//   * 先按 UTF-8 严格解码；成功即用（U+FFFD 只在它真的以 UTF-8 合法编码出现时才保留）；
//   * 失败且是「读取窗口末尾截断」时，去掉末尾最多 3 个未完成字节再试；
//   * 仍失败才回退本地编码（GBK 等）。
QString decodeTextStrict(const char *p, qint64 n, bool bWindowTruncated,
                         bool *pbFellBack, bool *pbTailTruncated)
{
    if (pbFellBack) *pbFellBack = false;
    if (pbTailTruncated) *pbTailTruncated = false;
    QByteArray bytes(p, qsizetype(n));

    // BOM：UTF-8 / UTF-16LE / UTF-16BE
    if (bytes.startsWith("\xEF\xBB\xBF"))
    {
        QStringDecoder dec(QStringDecoder::Utf8);
        const QString s = dec.decode(bytes.mid(3));
        if (!dec.hasError()) return s;
    }
    else if (bytes.startsWith("\xFF\xFE"))
    {
        QStringDecoder dec(QStringDecoder::Utf16LE);
        const QString s = dec.decode(bytes.mid(2));
        if (!dec.hasError()) return s;
    }
    else if (bytes.startsWith("\xFE\xFF"))
    {
        QStringDecoder dec(QStringDecoder::Utf16BE);
        const QString s = dec.decode(bytes.mid(2));
        if (!dec.hasError()) return s;
    }

    {
        QStringDecoder dec(QStringDecoder::Utf8);
        const QString s = dec.decode(bytes);
        if (!dec.hasError()) return s;
    }

    // 读取窗口在 1 MiB 边界截断：末尾可能是某个多字节字符的前半段。
    // 去掉最多 3 个尾字节后重新严格解码，成功就只丢弃这个不完整字符。
    if (bWindowTruncated)
    {
        for (int trim = 1; trim <= 3 && trim < bytes.size(); ++trim)
        {
            QStringDecoder dec(QStringDecoder::Utf8);
            const QString s = dec.decode(bytes.left(bytes.size() - trim));
            if (!dec.hasError())
            {
                if (pbTailTruncated) *pbTailTruncated = true;
                return s;
            }
        }
    }

    // 最后的兜底：本地编码（中文 Windows 为 GBK）。仍然可报告错误，
    // 避免把明显非文本的二进制内容当成合法文本显示。
    QStringDecoder local(QStringDecoder::System);
    const QString s = local.decode(bytes);
    if (pbFellBack) *pbFellBack = true;
    return s;
}

} // namespace

BuiltinViewer::BuiltinViewer(Mode mode)
    : m_mode(mode), m_widget(nullptr), m_title(nullptr), m_edit(nullptr)
{
    m_widget = new QWidget;
    auto *ly = new QVBoxLayout(m_widget);
    m_title = new QLabel(m_widget);
    m_title->setWordWrap(true);
    m_edit = new QPlainTextEdit(m_widget);
    m_edit->setReadOnly(true);
    m_edit->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono(QStringLiteral("Consolas"));
    mono.setStyleHint(QFont::TypeWriter);
    m_edit->setFont(mono);
    ly->addWidget(m_title);
    ly->addWidget(m_edit, 1);
}

BuiltinViewer::~BuiltinViewer()
{
    delete m_widget;   // 子控件由父对象递归释放
    m_widget = nullptr;
}

QWidget *BuiltinViewer::getPluginWidget() { return m_widget; }

void BuiltinViewer::setSourceName(const QString &name)
{
    m_sourceName = name;
    m_title->setText(QStringLiteral("%1 — %2")
                     .arg(m_mode == HexMode ? QStringLiteral("十六进制查看") : QStringLiteral("文本查看"),
                          name));
}

void BuiltinViewer::sendFileData(char *szFileData, qint64 nFileLen)
{
    if (szFileData == nullptr || nFileLen < 0)
    {
        m_edit->setPlainText(QStringLiteral("无法读取该文件内容。"));
        return;
    }
    const qint64 n = qMin(nFileLen, kMaxDisplayBytes);
    QString text;

    if (m_mode == TextMode)
    {
        bool bFellBack = false;
        bool bTailTruncated = false;
        QString s = decodeTextStrict(szFileData, n, nFileLen > n,
                                     &bFellBack, &bTailTruncated);
        text = s;
        if (bTailTruncated)
            text += QStringLiteral("\n[读取窗口在字符中间截断，已丢弃末尾不完整字符]");
        else if (bFellBack)
            text += QStringLiteral("\n[内容不是合法 UTF-8，已按本地编码显示]");
    }
    else
    {
        static const char hexdig[] = "0123456789ABCDEF";
        QString out;
        out.reserve(int(n / 16 + 1) * 78);
        for (qint64 off = 0; off < n; off += 16)
        {
            out += QStringLiteral("%1").arg(quint64(off), 8, 16, QChar('0')).toUpper();
            out += QStringLiteral("  ");
            QString ascii;
            for (int k = 0; k < 16; ++k)
            {
                const qint64 idx = off + k;
                if (idx < n)
                {
                    const unsigned char c = (unsigned char)szFileData[idx];
                    out += QChar(hexdig[c >> 4]);
                    out += QChar(hexdig[c & 0x0F]);
                    out += QChar(' ');
                    if (k == 7) out += QChar(' ');
                    ascii += (c >= 32 && c < 127) ? QChar(c) : QChar('.');
                }
                else
                {
                    out += QStringLiteral("   ");
                    if (k == 7) out += QChar(' ');
                }
            }
            out += QChar(' ');
            out += QChar('|');
            out += ascii;
            out += QChar('|');
            out += QChar('\n');
        }
        text = out;
    }

    if (nFileLen > kMaxDisplayBytes)
        text += QStringLiteral("\n[仅显示前 1 MB，文件总大小 %1 B；完整内容请用「导出文件」保存后查看]")
                .arg(nFileLen);
    m_edit->setPlainText(text);
}

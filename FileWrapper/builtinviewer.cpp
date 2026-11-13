#include "builtinviewer.h"

#include <QWidget>
#include <QLabel>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QFont>
#include <QByteArray>

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
        QString s = QString::fromUtf8(szFileData, qsizetype(n));
        // UTF-8 解码出现替换字符时回退到本地编码（中文 Windows 为 GBK）
        if (s.contains(QChar(0xFFFD)))
            s = QString::fromLocal8Bit(szFileData, qsizetype(n));
        text = s;
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

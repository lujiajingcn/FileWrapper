#include "mainwindow.h"
#include "ui_mainwindow.h"

// Qt6 已移除 QTextCodec；本地编码回退改用 QString::fromLocal8Bit
// P2-2：UTF-8 校验/解码统一交给 QStringDecoder（严格：拒绝过长编码、代理区、>U+10FFFF）
#include <QStringDecoder>
#include <QByteArrayView>
#include <QFileInfo>
#include <QLineEdit>
#include <QShortcut>
#include <QTextDocument>
#include <QToolBar>
#include <QStatusBar>
#include <QAction>
#include <QDebug>
#include <cstring>
#include <algorithm>
#include <limits>

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    // 查找栏：工具栏里放搜索框 + 上一个/下一个；Ctrl+F 聚焦搜索框，F3 / Shift+F3 循环查找
    auto *pSearch = new QLineEdit(this);
    pSearch->setPlaceholderText(tr("查找…"));
    pSearch->setClearButtonEnabled(true);
    pSearch->setMaximumWidth(240);
    ui->mainToolBar->addWidget(pSearch);
    QAction *pFindPrev = ui->mainToolBar->addAction(tr("上一个"));
    QAction *pFindNext = ui->mainToolBar->addAction(tr("下一个"));

    connect(pSearch, &QLineEdit::returnPressed, this, [this, pSearch]() {
        findText(pSearch->text(), true);
    });
    connect(pFindPrev, &QAction::triggered, this, [this, pSearch]() {
        findText(pSearch->text(), false);
    });
    connect(pFindNext, &QAction::triggered, this, [this, pSearch]() {
        findText(pSearch->text(), true);
    });

    auto *pScFind = new QShortcut(QKeySequence::Find, this);
    connect(pScFind, &QShortcut::activated, this, [pSearch]() {
        pSearch->setFocus();
        pSearch->selectAll();
    });
    auto *pScNext = new QShortcut(QKeySequence::FindNext, this);
    connect(pScNext, &QShortcut::activated, this, [this, pSearch]() {
        if (!pSearch->text().isEmpty()) findText(pSearch->text(), true);
    });
    auto *pScPrev = new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3), this);
    connect(pScPrev, &QShortcut::activated, this, [this, pSearch]() {
        if (!pSearch->text().isEmpty()) findText(pSearch->text(), false);
    });
}

// 在当前文本里查找；找不到时自动从头（或从尾）再找一次，实现循环查找
bool MainWindow::findText(const QString &sText, bool bForward)
{
    if (sText.isEmpty())
        return false;

    QTextDocument *pDoc = ui->textEdit->document();
    QTextCursor c = ui->textEdit->textCursor();
    QTextDocument::FindFlags flags;
    if (!bForward)
        flags |= QTextDocument::FindBackward;

    QTextCursor found = pDoc->find(sText, c, flags);
    if (found.isNull())
    {
        QTextCursor start(pDoc);
        if (!bForward)
            start.movePosition(QTextCursor::End);
        found = pDoc->find(sText, start, flags);
    }

    if (!found.isNull())
    {
        ui->textEdit->setTextCursor(found);
        ui->textEdit->ensureCursorVisible();
        statusBar()->showMessage(tr("找到：%1").arg(sText), 2000);
    }
    else
    {
        statusBar()->showMessage(tr("未找到：%1").arg(sText), 3000);
    }
    ui->textEdit->setFocus();
    return !found.isNull();
}

MainWindow::~MainWindow()
{
    delete ui;
}

// P2-2：严格 UTF-8 解码结果。
// bValid：整段都是合法 UTF-8；bTruncatedTail：末尾是"前缀合法但没写完"的多字节序列
// （可能是 1 MiB 读窗口的边界，也可能是文件被截断），此时 text 只含有效前缀，
// 调用方应提示续接/截断，而不是整篇回退本地编码变成乱码。
struct Utf8DecodeResult
{
    QString text;
    bool    bValid = false;
    bool    bTruncatedTail = false;
};

// P2-2：判断 data 末尾是否是"尚未写完但前缀合法"的 UTF-8 多字节序列，
// 返回该序列已出现的字节数（0 表示不是截断，而是确实存在非法字节）。
// 已经出现的续字节也按 UTF-8 的区间规则校验，避免把过长编码/代理区/
// > U+10FFFF 的非法序列误判成"只是没写完"。
static qsizetype incompleteUtf8Tail(QByteArrayView data)
{
    const qsizetype n = data.size();
    if (n == 0)
        return 0;
    // 一个多字节序列最多 4 字节，需要回看的残序列最多 3 字节（+首字节）
    for (qsizetype back = 1; back <= 4 && back <= n; ++back)
    {
        const qsizetype start = n - back;
        const unsigned char lead = static_cast<unsigned char>(data[start]);
        if ((lead & 0xC0) == 0x80)
            continue;                               // 续字节，继续往前找首字节
        int need = 0;
        if (lead >= 0xC2 && lead <= 0xDF)      need = 2;   // 0xC0/0xC1 为过长短编码
        else if (lead >= 0xE0 && lead <= 0xEF) need = 3;
        else if (lead >= 0xF0 && lead <= 0xF4) need = 4;   // 0xF5+ 超过 U+10FFFF
        else return 0;                              // 孤立续字节或非法首字节
        if (back >= need) return 0;                 // 序列已完整，说明不是截断
        // 已出现的续字节必须落在合法区间
        for (qsizetype i = start + 1; i < n; ++i)
        {
            const unsigned char c = static_cast<unsigned char>(data[i]);
            const qsizetype pos = i - start;        // 第几个续字节（1 起）
            unsigned char lo = 0x80, hi = 0xBF;
            if (pos == 1)
            {
                if (lead == 0xE0)      lo = 0xA0;   // 拒绝 3 字节过长编码
                else if (lead == 0xED) hi = 0x9F;   // 拒绝 UTF-16 代理区
                else if (lead == 0xF0) lo = 0x90;   // 拒绝 4 字节过长编码
                else if (lead == 0xF4) hi = 0x8F;   // 拒绝 > U+10FFFF
            }
            if (c < lo || c > hi)
                return 0;
        }
        return back;                                // 需要再补 back 个字节才完整
    }
    return 0;
}

// P2-2：严格 UTF-8 解码。
// 注意 Flag::Stateless：默认（有状态）模式下 Qt 会把末尾不完整序列静默挂起，
// 既不报错也不输出；Stateless 模式才会把"本段末尾的残序列"标记为错误，
// 从而能区分"中间非法字节"（必须回退本地编码）和"末尾截断"（只需解码有效前缀）。
static Utf8DecodeResult decodeUtf8Strict(QByteArrayView data)
{
    Utf8DecodeResult res;
    if (data.isEmpty())
    {
        res.bValid = true;
        return res;
    }

    QStringDecoder dec(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString text = dec.decode(data);
    if (!dec.hasError())
    {
        res.text   = text;
        res.bValid = true;
        return res;
    }

    // 整段解码报错：只有末尾是"合法但没写完"的多字节序列时才按截断处理，
    // 只解码有效前缀；中间有非法字节则返回失败，由调用方回退本地编码。
    const qsizetype tail = incompleteUtf8Tail(data);
    if (tail > 0)
    {
        QStringDecoder prefixDec(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
        QString prefix = prefixDec.decode(QByteArrayView(data.constData(), data.size() - tail));
        if (!prefixDec.hasError())
        {
            res.text           = prefix;
            res.bTruncatedTail = true;
            return res;
        }
    }
    res.text.clear();
    return res;
}

// 按编码解码文件内容为 QString：
//   1) 优先识别 BOM（UTF-8 / UTF-16LE / UTF-16BE）
//   2) 无 BOM 时，严格 UTF-8 校验通过则按 UTF-8 解析（覆盖绝大多数现代 .sql）
//   3) 末尾是截断的 UTF-8 多字节序列时，只解码有效前缀并置 pbTruncatedTail（P2-2）
//   4) 否则按系统本地编码解析（中文 Windows 上为 GBK），避免 ANSI/GBK 文件乱码
QString MainWindow::decodeText(const QByteArray &data, bool *pbTruncatedTail)
{
    if (pbTruncatedTail) *pbTruncatedTail = false;

    // 1) BOM 检测：BOM 已声明编码，直接按该编码解码。
    if (data.size() >= 3
        && (unsigned char)data[0] == 0xEF
        && (unsigned char)data[1] == 0xBB
        && (unsigned char)data[2] == 0xBF)
    {
        // P2-2：BOM 已声明是 UTF-8，即使中间有非法字节也按 UTF-8 替换字符输出，
        // 不再整篇回退本地编码（否则原本正确的中文会全部变成乱码）。
        const Utf8DecodeResult utf8 = decodeUtf8Strict(
            QByteArrayView(data.constData() + 3, data.size() - 3));
        if (pbTruncatedTail && utf8.bTruncatedTail) *pbTruncatedTail = true;
        if (utf8.bValid || utf8.bTruncatedTail)
            return utf8.text;
        qWarning() << "[PluginText] UTF-8 BOM 文件内含非法字节，已用替换字符解码";
        QStringDecoder dec(QStringDecoder::Utf8);
        return dec.decode(QByteArrayView(data.constData() + 3, data.size() - 3));
    }
    if (data.size() >= 2)
    {
        if ((unsigned char)data[0] == 0xFF && (unsigned char)data[1] == 0xFE) // UTF-16LE
            // Qt6 推荐 char16_t* 重载（ushort* 版本已废弃）；两者都是 16 位单元
            return QString::fromUtf16(reinterpret_cast<const char16_t *>(data.constData() + 2),
                                      (data.size() - 2) / 2);
        if ((unsigned char)data[0] == 0xFE && (unsigned char)data[1] == 0xFF) // UTF-16BE
        {
            QByteArray le = data.mid(2);
            unsigned char *b = reinterpret_cast<unsigned char *>(le.data());
            // le.size() 是 qsizetype（64 位）；用 int 计数在 >2 GiB 时会溢出为负，
            // 造成负下标越界写。
            for (qsizetype k = 0; k + 1 < le.size(); k += 2)
                std::swap(b[k], b[k + 1]);
            return QString::fromUtf16(reinterpret_cast<const char16_t *>(le.constData()), le.size() / 2);
        }
    }

    // 2) 无 BOM：严格 UTF-8 校验通过 -> UTF-8
    {
        // 放在局部作用域里：UTF-8 解码失败时它的 QString 先释放，
        // 再回退本地编码，避免 GBK 大文件同时占两份解码结果。
        const Utf8DecodeResult utf8 = decodeUtf8Strict(QByteArrayView(data));
        if (utf8.bValid)
            return utf8.text;
        if (utf8.bTruncatedTail)
        {
            // P2-2：末尾的残序列可能只是 1 MiB 读窗口的边界（由下一窗口续接），
            // 也可能文件真被截断；两种情况都只解码有效前缀，不在整篇里塞替换字符。
            if (pbTruncatedTail) *pbTruncatedTail = true;
            qWarning() << "[PluginText] 末尾 UTF-8 多字节序列不完整，已解码有效前缀";
            return utf8.text;
        }
    }

    // 3) 回退到系统本地编码（中文 Windows 上为 GBK）
    // Qt6 中 codecForLocale()->toUnicode() 等价于 QString::fromLocal8Bit()
    return QString::fromLocal8Bit(data);
}

void MainWindow::clearContent()
{
    ui->textEdit->clear();
}

void MainWindow::showText(char *szBuf, qint64 nFileLen)
{
    if (szBuf == nullptr || nFileLen <= 0)
        return;

    // 使用 nFileLen 作为长度（而非 strlen），避免读取越界或 NUL 截断。
    // Qt6 的 QByteArray 长度类型为 qsizetype（64 位），无需强转 int。
    // 64 位下 qsizetype 与 qint64 同宽，原「超过 qsizetype::max()」判断恒为
    // false（死代码）；这里改成真实的可显示上限，防止超大文本冻结 UI 线程。
    constexpr qint64 kMaxDisplayBytes = 64 * 1024 * 1024;   // 64 MiB
    if (nFileLen > kMaxDisplayBytes)
    {
        ui->textEdit->setPlainText("文件过大，无法载入内存显示。");
        return;
    }
    QByteArray array(szBuf, static_cast<qsizetype>(nFileLen));

    // P2-2：文本可能按 1 MiB 读窗口分块送入。长度正好是窗口整数倍时，末尾的
    // 残序列更可能是"窗口边界"（需与下一个窗口续接）而不是文件损坏；
    // 两种情况都只解码有效前缀，这里只区分提示文案。
    constexpr qint64 kReadWindowBytes = 1024 * 1024;   // 1 MiB
    const bool bWindowBoundary = (nFileLen >= kReadWindowBytes)
                                 && (nFileLen % kReadWindowBytes == 0);
    bool bTruncatedTail = false;
    const QString text = decodeText(array, &bTruncatedTail);
    if (bTruncatedTail)
    {
        statusBar()->showMessage(
            bWindowBoundary
                ? tr("文本在 1 MiB 读窗口末尾被截断，已解码有效前缀；不完整的尾序列需与后续数据续接。")
                : tr("文件末尾的 UTF-8 多字节序列不完整，已解码有效前缀（末尾可能有字符被截断）。"),
            8000);
    }

    // .html/.htm 交给 QTextEdit 的富文本渲染器（QTextBrowser 的子集）；
    // 其它文本（源码/日志/JSON…）仍用 setPlainText —— setText 会把标签吃掉。
    // 文件名未知（测试或外部直接送数据）时用内容嗅探兜底。
    const QString sExt = QFileInfo(m_sourceName).suffix().toLower();
    const bool bHtmlByExt = (sExt == QLatin1String("html") || sExt == QLatin1String("htm"));
    const bool bHtml = bHtmlByExt || (m_sourceName.isEmpty() && looksLikeHtml(text));

    if (bHtml)
        ui->textEdit->setHtml(text);
    else
        ui->textEdit->setPlainText(text);
}

// 粗略嗅探：只在扩展名未知时使用，宁可漏判（按纯文本显示）也不误判源码文件
bool MainWindow::looksLikeHtml(const QString &text)
{
    const QString head = text.left(2048).toLower();
    if (!head.trimmed().startsWith(QLatin1Char('<'))) return false;
    return head.contains(QStringLiteral("<!doctype html"))
           || head.contains(QStringLiteral("<html"))
           || (head.contains(QStringLiteral("<body")) && head.contains(QStringLiteral("</")));
}

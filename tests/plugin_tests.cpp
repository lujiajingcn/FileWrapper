// ============================================================================
// plugin_tests：插件基础设施 + PluginWord 的端到端验证（真实链路，不掺假）
//
// 覆盖的行为（断言标签前缀）：
//   W   后缀路由：用户映射 / 插件自描述 extensions / 未知后缀（C1）
//   W8  API 版本门禁（B1）：当前版本接受、旧插件拒绝、版本不符拒绝、空元数据拒绝
//   W9  加载失败的插件仍能路由后缀（sidecar 兜底，走"插件不可用"诊断）
//   W1b 启动时对加载失败的插件给出可见提示（A2）
//   B/C 真实 docx 渲染：段落/标题/字符格式/对齐/列表/表格合并/图片/超链接/中文编号
//   Z   缩放（缩小/放大/重置）
//   N   异常输入（旧版 .doc / rtf / 损坏 zip / 非 Word 压缩包 / 0 字节）+ 失败清屏
//   M   多标签页实例独立性
//   D   部署自检（清单比对、报告文本、失败登记、重试恢复、真实构建目录自检）
//   A   主程序链路：PluginManager 加载、QMergeFiles 合并、resolvePlugin 三步、
//       插件窗口挂进 QTabWidget、「插件诊断」入口存在
//
//   * 真实 FileManager 把真实 .docx 合并成 .dat 归档，再把字节读回来
//     （与程序里双击文件时的取数据路径一致）
//   * 真实插件 DLL，经 QPluginLoader 加载后走 PluginInterface
//   * 真实 QTextBrowser 文档：逐段/逐字符/逐表格/逐图片检查渲染结果
//
// 用法：plugin_tests <fixture-dir> <plugins-dir> <log-file> [shots-dir] [integrate]
//   fixture-dir  tests/fixtures（或重新生成后的目录）
//   plugins-dir  主程序 exe 同级的 plugins/ 目录（含已构建的插件 DLL 与 sidecar）
//   shots-dir    给出时额外保存渲染截图
//   integrate    追加 A 组（主程序 UI 链路）用例
//
// 断言标签一律用 ASCII，避免 Windows 控制台/脚本的编码干扰；日志按 UTF-8 写出。
// 一键运行见 tests/run-plugin-tests.ps1。
// ============================================================================

#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QModelIndex>
#include <QPixmap>
#include <QPluginLoader>
#include <QSettings>
#include <QStatusBar>
#include <QTreeView>
#include <QPushButton>
#include <QShortcut>
#include <QStandardItemModel>
#include <QItemSelectionModel>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextBrowser>
#include <QTextEdit>
#include <QLabel>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextImageFormat>
#include <QTextTable>
#include <QUrl>

#include <cstdio>
#include <functional>

#include "dlgpluginmap.h"
#include "filemanager.h"
#include "dlgmergefile.h"
#include "formatresolver.h"
#include "mainwindow.h"
#include "plugininterface.h"
#include "pluginmanager.h"
#include "plugindiag.h"

static int g_pass = 0;
static int g_fail = 0;
static QStringList g_log;

static void out(const QString &line)
{
    g_log << line;
    fprintf(stderr, "%s\n", line.toUtf8().constData());
    fflush(stderr);
}

static void report(bool ok, const QString &msg)
{
    if (ok) { ++g_pass; out(QStringLiteral("[PASS] ") + msg); }
    else    { ++g_fail; out(QStringLiteral("[FAIL] ") + msg); }
}

static void note(const QString &msg) { out(QStringLiteral("[INFO] ") + msg); }

static QByteArray readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return f.readAll();
}

// ---------------------------------------------------------------------------
// QTextDocument 检查辅助
// ---------------------------------------------------------------------------
static QTextBrowser *browserOf(QWidget *page)
{
    return page ? page->findChild<QTextBrowser *>() : nullptr;
}

static QTextDocument *documentOf(QWidget *page)
{
    QTextBrowser *b = browserOf(page);
    return b ? b->document() : nullptr;
}

static QTextBlock blockContaining(QTextDocument *doc, const QString &needle)
{
    if (!doc) return QTextBlock();
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        if (b.text().contains(needle)) return b;
    return QTextBlock();
}

static QTextCharFormat formatOfFragment(QTextDocument *doc, const QString &needle)
{
    if (!doc) return QTextCharFormat();
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        for (auto it = b.begin(); !it.atEnd(); ++it)
        {
            const QTextFragment f = it.fragment();
            if (f.isValid() && f.text().contains(needle)) return f.charFormat();
        }
    return QTextCharFormat();
}

static qreal blockFontSize(const QTextBlock &b)
{
    if (!b.isValid()) return 0.0;
    for (auto it = b.begin(); !it.atEnd(); ++it)
    {
        const QTextFragment f = it.fragment();
        if (f.isValid() && !f.text().trimmed().isEmpty())
            return f.charFormat().fontPointSize();
    }
    return 0.0;
}

static QList<QTextTable *> tablesOf(QTextDocument *doc)
{
    QList<QTextTable *> out;
    if (!doc) return out;
    std::function<void(QTextFrame *)> walk = [&](QTextFrame *f) {
        if (!f) return;
        for (QTextFrame *c : f->childFrames())
        {
            if (QTextTable *t = qobject_cast<QTextTable *>(c)) out.append(t);
            walk(c);
        }
    };
    walk(doc->rootFrame());
    return out;
}

static QString cellText(QTextDocument *doc, const QTextTableCell &cell)
{
    if (!doc) return QString();
    QTextCursor cur(doc);
    cur.setPosition(cell.firstPosition());
    cur.setPosition(cell.lastPosition(), QTextCursor::KeepAnchor);
    return cur.selectedText();
}

static QTextImageFormat firstImageFormat(QTextDocument *doc)
{
    if (!doc) return QTextImageFormat();
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        for (auto it = b.begin(); !it.atEnd(); ++it)
        {
            const QTextCharFormat f = it.fragment().charFormat();
            if (f.isImageFormat()) return f.toImageFormat();
        }
    return QTextImageFormat();
}

static bool hasAnchorHref(QTextDocument *doc, const QString &href)
{
    if (!doc) return false;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        for (auto it = b.begin(); !it.atEnd(); ++it)
            if (it.fragment().charFormat().anchorHref() == href) return true;
    return false;
}

static QString statusMessage(QWidget *page)
{
    if (!page) return QString();
    QStatusBar *sb = page->findChild<QStatusBar *>();
    return sb ? sb->currentMessage() : QString();
}

static void saveShot(QWidget *page, const QString &path)
{
    if (!page) { report(false, QStringLiteral("S: null widget, skip shot")); return; }
    page->resize(860, 940);
    page->show();
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 500) QApplication::processEvents();
    const QPixmap pix = page->grab();
    report(!pix.isNull() && pix.save(path),
           QStringLiteral("S: saved %1 (%2x%3)")
               .arg(QFileInfo(path).fileName()).arg(pix.width()).arg(pix.height()));
}

// ---------------------------------------------------------------------------
// W1: suffix -> plugin mapping（走主程序真实的 choosePlugin）
// ---------------------------------------------------------------------------
class ProbeMainWindow : public MainWindow
{
public:
    ProbeMainWindow() : MainWindow(nullptr) {}
    using MainWindow::choosePlugin;
};

static void checkExtensionMapping()
{
    struct Case { const char *ext; const char *dll; };
    static const Case kCases[] = {
        { "docx", "PluginWord" }, { "docm", "PluginWord" }, { "dotx", "PluginWord" },
        { "dotm", "PluginWord" }, { "doc",  "PluginWord" }, { "DOCX", "PluginWord" },
        { "odt",  "PluginWord" }, { "ott",  "PluginWord" }, { "rtf", "PluginWord" },
        { "txt",  "PluginText" },    { "md",   "PluginText" },
        { "vtt",  "PluginText" },    { "srt",  "PluginText" },
        { "jpg",  "PluginPicture" }, { "png",  "PluginPicture" },
        { "svg",  "PluginPicture" }, { "webp", "PluginPicture" },
        { "tiff", "PluginPicture" },
        { "mp4",  "PluginMedia" },   { "mp3",  "PluginMedia" },
        { "m2ts", "PluginMedia" },
        { "pdf",  "PluginPdf" },
        { "xlsx", "PluginSheet" },   { "ods",  "PluginSheet" },
        { "pptx", "PluginSlides" },  { "odp",  "PluginSlides" },
        { "epub", "PluginBook" },    { "fb2",  "PluginBook" },
        { "qzx",  nullptr },
    };

    ProbeMainWindow mw;
    for (const Case &c : kCases)
    {
        const QString ext = QString::fromLatin1(c.ext);
        if (!DlgPluginMap::mappingFor(ext).isEmpty())
        {
            note(QStringLiteral("W1: user mapping exists for .%1, skipped").arg(ext));
            continue;
        }
        const QString path = mw.choosePlugin(ext);
        const QString actual = QFileInfo(path).fileName();
        if (c.dll == nullptr)
        {
            report(path.isEmpty(), QStringLiteral("W1: unknown .%1 -> no plugin (got '%2')").arg(ext, actual));
        }
        else
        {
            const QString expect = QString::fromLatin1(c.dll) + QStringLiteral(".dll");
            report(actual.compare(expect, Qt::CaseInsensitive) == 0,
                   QStringLiteral("W1: .%1 -> %2 (got %3)").arg(ext, expect, actual));
        }
    }

    // W1b: 有插件加载失败时，启动阶段应给出用户可见提示（状态栏），而不是只进日志
    if (QStatusBar *sb = mw.findChild<QStatusBar *>())
    {
        const bool bAnyFailure = !PluginManager::getInstance()->failures().isEmpty();
        report(!bAnyFailure || sb->currentMessage().contains(QStringLiteral("插件")),
               QStringLiteral("W1b: startup hint with failed plugin present ('%1')")
                   .arg(sb->currentMessage()));
    }
}

// ---------------------------------------------------------------------------
// B: basic.docx 渲染
// ---------------------------------------------------------------------------
static void verifyBasicDoc(QWidget *page)
{
    QTextDocument *doc = documentOf(page);
    report(doc != nullptr, QStringLiteral("B01: document object"));
    if (!doc) return;

    const QString txt = doc->toPlainText();
    report(txt.contains(QString::fromUtf8("一级标题：测试文档标题")), QStringLiteral("B02: heading text"));
    report(txt.contains(QString::fromUtf8("第一段普通正文")), QStringLiteral("B03: body text"));
    report(txt.contains(QString::fromUtf8("合并表头")) && txt.contains(QStringLiteral("R2C3")),
           QStringLiteral("B04: table text"));
    report(txt.contains(QString::fromUtf8("参考链接：示例站点")), QStringLiteral("B05: hyperlink text"));

    const int iTitle = txt.indexOf(QString::fromUtf8("一级标题"));
    const int iBody  = txt.indexOf(QString::fromUtf8("第一段普通正文"));
    const int iList  = txt.indexOf(QString::fromUtf8("编号第一项"));
    report(iTitle >= 0 && iTitle < iBody && iBody < iList,
           QStringLiteral("B06: paragraph order %1 < %2 < %3").arg(iTitle).arg(iBody).arg(iList));

    const qreal szTitle = blockFontSize(blockContaining(doc, QString::fromUtf8("一级标题")));
    const qreal szBody  = blockFontSize(blockContaining(doc, QString::fromUtf8("第一段普通正文")));
    report(szTitle > szBody && szBody > 0.0,
           QStringLiteral("B07: heading %1pt > body %2pt").arg(szTitle).arg(szBody));

    report(formatOfFragment(doc, QString::fromUtf8("粗体文字")).fontWeight() >= QFont::Bold,
           QStringLiteral("B08: bold"));
    report(formatOfFragment(doc, QString::fromUtf8("斜体文字")).fontItalic(), QStringLiteral("B09: italic"));
    report(formatOfFragment(doc, QString::fromUtf8("下划线文字")).fontUnderline(), QStringLiteral("B10: underline"));
    report(formatOfFragment(doc, QString::fromUtf8("删除线文字")).fontStrikeOut(), QStringLiteral("B11: strike"));

    const QTextCharFormat fRed = formatOfFragment(doc, QString::fromUtf8("红色大字"));
    report(fRed.foreground().color().name().toUpper() == QStringLiteral("#C00000"),
           QStringLiteral("B12: color %1").arg(fRed.foreground().color().name()));
    report(qAbs(fRed.fontPointSize() - 18.0) < 0.01,
           QStringLiteral("B13: size %1pt").arg(fRed.fontPointSize()));
    report(formatOfFragment(doc, QString::fromUtf8("，上标x")).verticalAlignment() == QTextCharFormat::AlignSuperScript,
           QStringLiteral("B14: superscript"));
    report(formatOfFragment(doc, QString::fromUtf8("，下标y")).verticalAlignment() == QTextCharFormat::AlignSubScript,
           QStringLiteral("B15: subscript"));

    const QTextBlock bCenter = blockContaining(doc, QString::fromUtf8("居中对齐段落"));
    report(bCenter.isValid() && bCenter.blockFormat().alignment().testFlag(Qt::AlignHCenter),
           QStringLiteral("B16: center aligned"));
    const QTextBlock bRight = blockContaining(doc, QString::fromUtf8("右对齐段落"));
    report(bRight.isValid() && bRight.blockFormat().alignment().testFlag(Qt::AlignRight),
           QStringLiteral("B17: right aligned"));

    const QTextBlock bNum = blockContaining(doc, QString::fromUtf8("编号第一项"));
    report(bNum.isValid() && bNum.text().trimmed().startsWith(QLatin1Char('1')),
           QStringLiteral("B18: list prefix '%1'").arg(bNum.text().left(6)));
    report(bNum.isValid() && bNum.blockFormat().leftMargin() > 0.0,
           QStringLiteral("B19: list left margin %1px").arg(bNum.blockFormat().leftMargin()));
    const QTextBlock bBullet = blockContaining(doc, QString::fromUtf8("项目第一项"));
    report(bBullet.isValid() && bBullet.text().contains(QChar(0x2022)),
           QStringLiteral("B20: bullet char '%1'").arg(bBullet.text().left(5)));

    const QList<QTextTable *> tables = tablesOf(doc);
    report(tables.size() == 1, QStringLiteral("B21: table count %1").arg(tables.size()));
    if (tables.size() == 1)
    {
        QTextTable *t = tables.first();
        report(t->rows() == 3 && t->columns() == 4,
               QStringLiteral("B22: grid %1x%2").arg(t->rows()).arg(t->columns()));
        report(t->cellAt(0, 0).firstPosition() == t->cellAt(0, 1).firstPosition(),
               QStringLiteral("B23: horizontal merge (colspan)"));
        report(t->cellAt(1, 0).firstPosition() == t->cellAt(2, 0).firstPosition(),
               QStringLiteral("B24: vertical merge (rowspan)"));
        report(cellText(doc, t->cellAt(0, 0)).contains(QString::fromUtf8("合并表头")),
               QStringLiteral("B25: merged cell text"));
        report(cellText(doc, t->cellAt(2, 3)).contains(QStringLiteral("R2C3")),
               QStringLiteral("B26: last cell text"));
    }

    const QTextImageFormat imf = firstImageFormat(doc);
    const QImage img = imf.isValid()
            ? doc->resource(QTextDocument::ImageResource, QUrl(imf.name())).value<QImage>()
            : QImage();
    report(!img.isNull(), QStringLiteral("B27: embedded image %1x%2 url=%3")
               .arg(img.width()).arg(img.height()).arg(imf.name()));
    report(imf.isValid() && imf.width() > 100.0 && imf.width() < 200.0,
           QStringLiteral("B28: image display width %1px (1.5in=144px)").arg(imf.width()));
    report(hasAnchorHref(doc, QStringLiteral("https://example.com/doc")),
           QStringLiteral("B29: hyperlink target kept"));

    report(page && page->windowTitle().contains(QStringLiteral("basic.docx")),
           QStringLiteral("B30: window title '%1'").arg(page ? page->windowTitle() : QString()));
    const QString status = statusMessage(page);
    report(status.contains(QString::fromUtf8("段")) && status.contains(QString::fromUtf8("图片 1")),
           QStringLiteral("B31: status bar '%1'").arg(status));
}

// ---------------------------------------------------------------------------
// C: chinese.docx 渲染（中文样式名 / 中文编号 / 合并单元格）
// ---------------------------------------------------------------------------
static void verifyChineseDoc(QWidget *page)
{
    QTextDocument *doc = documentOf(page);
    report(doc != nullptr, QStringLiteral("C01: document object"));
    if (!doc) return;

    const QString txt = doc->toPlainText();
    report(txt.contains(QString::fromUtf8("中文样式标题")), QStringLiteral("C02: localized heading text"));

    const QTextBlock b1 = blockContaining(doc, QString::fromUtf8("中文编号第一项"));
    const QTextBlock b2 = blockContaining(doc, QString::fromUtf8("中文编号第二项"));
    const QTextBlock b3 = blockContaining(doc, QString::fromUtf8("中文编号第三项"));
    report(b1.isValid() && b1.text().startsWith(QString::fromUtf8("一、")),
           QStringLiteral("C03: cn number 1 -> '%1'").arg(b1.text().left(6)));
    report(b2.isValid() && b2.text().startsWith(QString::fromUtf8("二、")),
           QStringLiteral("C04: cn number 2 -> '%1'").arg(b2.text().left(6)));
    report(b3.isValid() && b3.text().startsWith(QString::fromUtf8("三、")),
           QStringLiteral("C05: cn number 3 -> '%1'").arg(b3.text().left(6)));

    const QTextBlock sb1 = blockContaining(doc, QString::fromUtf8("二级编号甲"));
    report(sb1.isValid() && sb1.text().contains(QString::fromUtf8("（1）")),
           QStringLiteral("C06: level-2 number -> '%1'").arg(sb1.text().left(8)));

    const QTextBlock bTitle = blockContaining(doc, QString::fromUtf8("中文样式标题"));
    report(qAbs(blockFontSize(bTitle) - 22.0) < 0.01,
           QStringLiteral("C07: localized heading size %1pt (sz=44 half-pt)")
               .arg(blockFontSize(bTitle)));
    report(formatOfFragment(doc, QString::fromUtf8("中文样式标题")).fontWeight() >= QFont::Bold,
           QStringLiteral("C08: localized heading bold"));

    const QTextCharFormat fRich = formatOfFragment(doc, QString::fromUtf8("加粗斜体下划线绿字"));
    report(fRich.fontWeight() >= QFont::Bold && fRich.fontItalic() && fRich.fontUnderline(),
           QStringLiteral("C09: bold+italic+underline"));
    report(fRich.foreground().color().name().toUpper() == QStringLiteral("#00B050"),
           QStringLiteral("C10: color %1").arg(fRich.foreground().color().name()));
    report(fRich.background().color().name().toUpper() == QStringLiteral("#FFFF00"),
           QStringLiteral("C11: highlight %1").arg(fRich.background().color().name()));

    const QTextBlock bCenter = blockContaining(doc, QString::fromUtf8("居中且首行缩进的段落"));
    report(bCenter.isValid() && bCenter.blockFormat().alignment().testFlag(Qt::AlignHCenter),
           QStringLiteral("C12: center aligned"));
    report(bCenter.isValid() && bCenter.blockFormat().textIndent() > 0.0,
           QStringLiteral("C13: first-line indent %1px").arg(bCenter.blockFormat().textIndent()));

    const QTextBlock bBody = blockContaining(doc, QString::fromUtf8("表格之后的收尾段落"));
    report(bBody.isValid()
               && bBody.blockFormat().lineHeightType() == QTextBlockFormat::ProportionalHeight
               && qAbs(bBody.blockFormat().lineHeight() - 150.0) < 1.0,
           QStringLiteral("C14: line height %1%")
               .arg(bBody.isValid() ? bBody.blockFormat().lineHeight() : 0.0));

    const QList<QTextTable *> tables = tablesOf(doc);
    report(tables.size() == 1, QStringLiteral("C15: table count %1").arg(tables.size()));
    if (tables.size() == 1)
    {
        QTextTable *t = tables.first();
        report(t->rows() == 3 && t->columns() == 2,
               QStringLiteral("C16: grid %1x%2").arg(t->rows()).arg(t->columns()));
        report(t->cellAt(0, 0).firstPosition() == t->cellAt(0, 1).firstPosition(),
               QStringLiteral("C17: gridSpan merge"));
        report(t->cellAt(1, 0).firstPosition() == t->cellAt(2, 0).firstPosition(),
               QStringLiteral("C18: vMerge merge"));
        report(cellText(doc, t->cellAt(1, 0)).contains(QString::fromUtf8("纵向跨行")),
               QStringLiteral("C19: merged cell text"));
        report(cellText(doc, t->cellAt(2, 1)).contains(QStringLiteral("RB2")),
               QStringLiteral("C20: remaining cell text"));
    }
}

// ---------------------------------------------------------------------------
// Z: zoom
// ---------------------------------------------------------------------------
static void verifyZoom(QWidget *page)
{
    QTextDocument *doc = documentOf(page);
    QAction *zoomIn = page ? page->findChild<QAction *>(QStringLiteral("actionZoomIn")) : nullptr;
    QAction *zoomReset = page ? page->findChild<QAction *>(QStringLiteral("actionZoomReset")) : nullptr;
    report(zoomIn != nullptr && zoomReset != nullptr, QStringLiteral("Z1: zoom actions exist"));
    if (!doc || !zoomIn || !zoomReset) return;

    const qreal before = blockFontSize(blockContaining(doc, QString::fromUtf8("第一段普通正文")));
    for (int i = 0; i < 3; ++i) zoomIn->trigger();
    const qreal after = blockFontSize(blockContaining(doc, QString::fromUtf8("第一段普通正文")));
    report(before > 0.0 && after > before * 1.2,
           QStringLiteral("Z2: zoom in %1pt -> %2pt").arg(before).arg(after));
    report(statusMessage(page).contains(QStringLiteral("133")),
           QStringLiteral("Z3: status '%1'").arg(statusMessage(page).left(30)));

    zoomReset->trigger();
    const qreal back = blockFontSize(blockContaining(doc, QString::fromUtf8("第一段普通正文")));
    report(qAbs(back - before) < 0.05, QStringLiteral("Z4: reset back to %1pt").arg(back));
}

// ---------------------------------------------------------------------------
// N: 异常输入
// ---------------------------------------------------------------------------
static void verifyErrorPaths(PluginInterface *tmpl, const QString &work)
{
    struct Neg { const char *file; const char *expect; const char *label; };
    static const Neg kNegs[] = {
        { "legacy.doc",   "\u53e6\u5b58\u4e3a",            "legacy .doc -> save-as hint" },
        { "broken.docx",  "docx",                           "broken zip -> parse error" },
        { "notword.docx", "word/document.xml",              "non-word zip -> missing part" },
        { "empty.docx",   "\u6587\u4ef6\u4e3a\u7a7a",       "0 byte -> empty hint" },
    };

    for (const Neg &n : kNegs)
    {
        const QByteArray data = readFile(work + QLatin1Char('/') + QLatin1String(n.file));
        PluginInterface *p = tmpl->createInstance();
        if (!p) { report(false, QStringLiteral("N: create failed for %1").arg(QLatin1String(n.label))); continue; }
        QByteArray copy = data;
        p->sendFileData(copy.isEmpty() ? nullptr : copy.data(), copy.size());

        QTextDocument *d = documentOf(p->getPluginWidget());
        const QString t = d ? d->toPlainText() : QString();
        report(t.contains(QString::fromUtf8(n.expect)),
               QStringLiteral("N: %1 -> '%2'")
                   .arg(QString::fromLatin1(n.label), t.simplified().left(40)));
        delete p;
    }

    // 打开内容后再送 (nullptr,0)：旧内容必须被清掉
    PluginInterface *p = tmpl->createInstance();
    QByteArray data = readFile(work + QStringLiteral("/basic.docx"));
    p->sendFileData(data.data(), data.size());
    QTextDocument *d = documentOf(p->getPluginWidget());
    const bool had = d && d->toPlainText().contains(QString::fromUtf8("一级标题"));
    p->sendFileData(nullptr, 0);
    d = documentOf(p->getPluginWidget());
    const QString after = d ? d->toPlainText() : QString();
    report(had && !after.contains(QString::fromUtf8("一级标题"))
               && after.contains(QString::fromUtf8("文件为空")),
           QStringLiteral("N: stale content cleared on failure path"));
    delete p;
}

// ---------------------------------------------------------------------------
// M: 多实例独立
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// D: 部署自检与插件失败可见性（A1/A2 的实现验证）
//
// 注意：PluginManager 是单例，且主程序 MainWindow 构造时就会扫描一次插件；
// 因此「坏插件」必须在任何 MainWindow 被创建之前放进 plugins/ 目录，
// 才能验证到"加载失败被记录"。见 installBrokenPluginForScan()。
// ---------------------------------------------------------------------------
static QString brokenProbePluginPath()
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/plugins/ZZBroken.dll");
}

static void verifyDeploymentDiagnostics(const QString &appPluginsDir)
{
    // ---- D1/D2: 清单解析、缺失项、无清单时的降级 ----
    QTemporaryDir tmp;
    report(tmp.isValid(), QStringLiteral("D1: temp app dir"));
    if (!tmp.isValid()) return;

    const QString appDir = tmp.path();
    {
        QFile existing(appDir + QStringLiteral("/PresentDep.dll"));
        existing.open(QIODevice::WriteOnly);
        existing.write("x");
    }
    {
        QFile mf(PluginDiagnostics::manifestPath(appDir));
        mf.open(QIODevice::WriteOnly | QIODevice::Truncate);
        mf.write("PresentDep.dll|already present\n"
                 "MissingDep.dll|this dep does something important\n");
    }

    QString sErr;
    const QList<PluginDiagnostics::MissingFile> missing =
            PluginDiagnostics::missingFiles(appDir, &sErr);
    report(sErr.isEmpty() && missing.size() == 1
               && missing[0].path == QStringLiteral("MissingDep.dll")
               && missing[0].hint.contains(QStringLiteral("important")),
           QStringLiteral("D2: manifest diff -> 1 missing (%1)")
               .arg(missing.isEmpty() ? QStringLiteral("-") : missing[0].path));

    // 没有清单时要能明确报错，而不是静默当成"没问题"
    QTemporaryDir tmpNoManifest;
    QString sNoManifestErr;
    PluginDiagnostics::missingFiles(tmpNoManifest.path(), &sNoManifestErr);
    report(!sNoManifestErr.isEmpty(), QStringLiteral("D3: no manifest -> explicit error"));

    // ---- D4: 失败报告文本（插件名 + 原始错误 + 缺失项 + 用途）----
    const QString reportText = PluginDiagnostics::buildReport(
                appDir + QStringLiteral("/plugins/FooPlugin.dll"),
                QStringLiteral("Cannot load library ...: the specified module could not be found."),
                appDir);
    report(reportText.contains(QStringLiteral("FooPlugin.dll"))
               && reportText.contains(QStringLiteral("Cannot load library"))
               && reportText.contains(QStringLiteral("MissingDep.dll"))
               && reportText.contains(QStringLiteral("important")),
           QStringLiteral("D4: report has plugin name + error + missing file + hint"));

    // ---- D8: 真实构建目录的部署自检（比对清单与实际文件）----
    // appPluginsDir 形如 <构建>/FileWrapper/debug/plugins，其父目录就是程序目录。
    const QString sRealAppDir = QFileInfo(appPluginsDir).absolutePath();
    QString sRealErr;
    const QList<PluginDiagnostics::MissingFile> realMissing =
            PluginDiagnostics::missingFiles(sRealAppDir, &sRealErr);
    report(sRealErr.isEmpty() && realMissing.isEmpty(),
           QStringLiteral("D8: real deploy dir matches manifest (%1)")
               .arg(sRealErr.isEmpty()
                    ? (realMissing.isEmpty() ? QStringLiteral("all present")
                                             : QStringLiteral("missing ") + realMissing.first().path)
                    : sRealErr));

    // ---- D5: MainWindow 构造时的扫描已把坏插件记为失败 ----
    PluginManager *pm = PluginManager::getInstance();
    if (pm->failures().isEmpty())
        pm->loadAllPlugins();          // 兜底（正常情况下已被 MainWindow 触发过）

    QStringList sFailurePaths;
    for (const PluginManager::Failure &f : pm->failures())
        sFailurePaths << f.path;
    const QString sBrokenError = pm->failureFor(brokenProbePluginPath());
    report(!sBrokenError.isEmpty() && pm->getAllInterfaces().size() >= 6,
           QStringLiteral("D5: %1 loaded, failure recorded: [%2] error='%3'")
               .arg(pm->getAllInterfaces().size())
               .arg(sFailurePaths.join(QStringLiteral(", ")))
               .arg(sBrokenError.left(40)));

    // ---- D6: 把坏文件换成真插件后「重试加载」能恢复 ----
    // 用 PluginPdf.dll 当替身：它只声明 1 个后缀，注册表冲突告警最少
    // （用 PluginText.dll 会刷 37 条 "后缀被多个插件声明" 的告警）。
    const QString sPath = brokenProbePluginPath();
    QFile::remove(sPath);
    QFile::copy(appPluginsDir + QStringLiteral("/PluginPdf.dll"), sPath);
    const int recovered = pm->retryFailedPlugins();
    report(recovered == 1 && pm->failures().isEmpty()
               && pm->getInterface(sPath) != nullptr,
           QStringLiteral("D6: retry recovered %1 plugin").arg(recovered));

    // ---- D7: 文件损坏时重试不会假装成功 ----
    // 注意必须换一个"本进程从未加载过"的文件名：同一个路径在 D6 已被成功加载，
    // Windows 会把 QLibrary 的模块句柄缓存下来，再查一次不会是 nullptr。
    const QString sStillBadPath = QCoreApplication::applicationDirPath()
                                  + QStringLiteral("/plugins/ZZStillBad.dll");
    QFile::remove(sStillBadPath);
    {
        QFile f(sStillBadPath);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write("still not a plugin");
    }
    {
        QPluginLoader probe(sStillBadPath);
        report(probe.instance() == nullptr, QStringLiteral("D7: broken dll cannot be loaded"));
    }
    QFile::remove(sStillBadPath);
    QFile::remove(sPath);                  // 收尾：不影响后续用例的插件计数
}

// 在真实 MainWindow 触发插件扫描之前，放一个假插件进去。
// 同时写一份 sidecar 元数据（C1）：用来验证「插件加载失败时仍能从 sidecar
// 知道它声明了哪些后缀」，从而给出「插件不可用」而不是「未找到插件」。
static void installBrokenPluginForScan()
{
    const QString sPath = brokenProbePluginPath();
    QFile::remove(sPath);
    QFile f(sPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write("this is not a valid plugin dll");

    const QString sSidecar = QFileInfo(sPath).dir().filePath(QStringLiteral("ZZBroken.json"));
    QFile j(sSidecar);
    if (j.open(QIODevice::WriteOnly | QIODevice::Truncate))
        j.write("{\"Name\":\"zzBroken\",\"apiVersion\":2,\"extensions\":[\"zzz\"]}");
}

// ---------------------------------------------------------------------------
// W2-W7: C1（插件自描述后缀）+ B1（API 版本协商）
// ---------------------------------------------------------------------------
static void verifyExtensionRegistry(const QString &appPluginsDir)
{
    PluginManager *pm = PluginManager::getInstance();

    // ---- W2: 后缀来自插件自己的 JSON，而不是主程序硬编码表 ----
    const QString sWord = pm->pluginForExtension(QStringLiteral("docx"));
    report(QFileInfo(sWord).fileName().compare(QStringLiteral("PluginWord.dll"), Qt::CaseInsensitive) == 0,
           QStringLiteral("W2: docx -> %1 (declared by plugin metadata)")
               .arg(QFileInfo(sWord).fileName()));

    const QString sPdf = pm->pluginForExtension(QStringLiteral("pdf"));
    const QString sEpub = pm->pluginForExtension(QStringLiteral("epub"));
    report(QFileInfo(sPdf).fileName().startsWith(QStringLiteral("PluginPdf"), Qt::CaseInsensitive)
               && QFileInfo(sEpub).fileName().startsWith(QStringLiteral("PluginBook"), Qt::CaseInsensitive),
           QStringLiteral("W3: pdf -> %1, epub -> %2")
               .arg(QFileInfo(sPdf).fileName(), QFileInfo(sEpub).fileName()));

    // ---- W4: 原硬编码表共 90 项，全部由插件 JSON 接管 ----
    // 90 项声明里有 1 个跨插件重名（".ts" = MPEG-TS 视频 vs TypeScript 文本），
    // 因此唯一路由数是 112，冲突按 extensionPriority 决出（PluginText 声明 10）。
    report(pm->registeredExtensionCount() >= 112,
           QStringLiteral("W4: %1 unique extensions registered (113 declared; .ts is shared)")
               .arg(pm->registeredExtensionCount()));
    report(QFileInfo(pm->pluginForExtension(QStringLiteral("ts"))).fileName()
               .compare(QStringLiteral("PluginText.dll"), Qt::CaseInsensitive) == 0,
           QStringLiteral("W4b: .ts -> %1 (extensionPriority resolves the MPEG-TS/TypeScript clash)")
               .arg(QFileInfo(pm->pluginForExtension(QStringLiteral("ts"))).fileName()));

    // ---- W5: 大小写 / 带前导点都应当生效 ----
    report(!sWord.isEmpty()
               && pm->pluginForExtension(QStringLiteral(".DOCX")) == sWord,
           QStringLiteral("W5: case-insensitive and leading-dot tolerant"));

    // ---- W6: 每个插件的 sidecar 都声明了 apiVersion == PLUGIN_API_VERSION ----
    int nBadVersion = 0;
    int nSidecars = 0;
    for (const QString &sFile : QDir(appPluginsDir).entryList(QStringList() << QStringLiteral("*.json"),
                                                              QDir::Files))
    {
        QFile f(QDir(appPluginsDir).filePath(sFile));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        if (!doc.isObject()) { ++nBadVersion; continue; }
        ++nSidecars;
        if (doc.object().value(QStringLiteral("apiVersion")).toInt() != PLUGIN_API_VERSION)
            ++nBadVersion;
    }
    report(nSidecars >= 6 && nBadVersion == 0,
           QStringLiteral("W6: %1 sidecar(s), all apiVersion=%2 (bad=%3)")
               .arg(nSidecars).arg(PLUGIN_API_VERSION).arg(nBadVersion));

    // ---- W7: 用户映射优先，且能解析逻辑名 -> 真实路径 ----
    ProbeMainWindow mw;
    DlgPluginMap::removeMapping(QStringLiteral("qzx"));
    DlgPluginMap::setMapping(QStringLiteral("qzx"), QStringLiteral("PluginText"));
    const QString sMapped = mw.choosePlugin(QStringLiteral("qzx"));
    DlgPluginMap::setMapping(QStringLiteral("qzx"), QStringLiteral("builtin://hex"));
    const QString sBuiltin = mw.choosePlugin(QStringLiteral("qzx"));
    DlgPluginMap::setMapping(QStringLiteral("qzx"), QStringLiteral("none"));
    const QString sNone = mw.choosePlugin(QStringLiteral("qzx"));
    DlgPluginMap::removeMapping(QStringLiteral("qzx"));
    report(QFileInfo(sMapped).fileName().compare(QStringLiteral("PluginText.dll"), Qt::CaseInsensitive) == 0
               && sBuiltin == QStringLiteral("builtin://hex") && sNone.isEmpty(),
           QStringLiteral("W7: user mapping wins (text='%1' builtin='%2' none='%3')")
               .arg(QFileInfo(sMapped).fileName(), sBuiltin, sNone));
}

// ---------------------------------------------------------------------------
// W8: B1 版本协商（纯函数用例，不需要造旧 DLL）
// ---------------------------------------------------------------------------
static void verifyApiVersionGate()
{
    QString sErr;
    int nFound = 0;

    // 当前版本：接受
    QJsonObject good;
    good.insert(QStringLiteral("apiVersion"), PLUGIN_API_VERSION);
    QJsonObject goodWrapper;
    goodWrapper.insert(QStringLiteral("MetaData"), good);
    report(PluginManager::apiVersionAcceptable(goodWrapper, &sErr),
           QStringLiteral("W8a: apiVersion=%1 accepted").arg(PLUGIN_API_VERSION));

    // 旧插件：元数据里没有 apiVersion
    QJsonObject legacy;
    legacy.insert(QStringLiteral("Name"), QStringLiteral("oldPlugin"));
    QJsonObject legacyWrapper;
    legacyWrapper.insert(QStringLiteral("MetaData"), legacy);
    sErr.clear();
    report(!PluginManager::apiVersionAcceptable(legacyWrapper, &sErr)
               && sErr.contains(QStringLiteral("apiVersion")),
           QStringLiteral("W8b: legacy plugin (no apiVersion) rejected"));

    // 版本不匹配：拒绝，并把实际版本回报出来
    QJsonObject newer;
    newer.insert(QStringLiteral("apiVersion"), PLUGIN_API_VERSION + 1);
    QJsonObject newerWrapper;
    newerWrapper.insert(QStringLiteral("MetaData"), newer);
    sErr.clear();
    nFound = 0;
    const bool bNewerRejected = !PluginManager::apiVersionAcceptable(newerWrapper, &sErr, &nFound);
    report(bNewerRejected && nFound == PLUGIN_API_VERSION + 1,
           QStringLiteral("W8c: apiVersion=%1 rejected (found=%2)")
               .arg(PLUGIN_API_VERSION + 1).arg(nFound));

    // 完全不是本程序插件：元数据为空
    sErr.clear();
    const bool bEmptyRejected = !PluginManager::apiVersionAcceptable(QJsonObject(), &sErr);
    report(bEmptyRejected && !sErr.isEmpty(),
           QStringLiteral("W8d: empty metadata rejected ('%1')").arg(sErr.left(20)));
}

// ---------------------------------------------------------------------------
// W9: 加载失败的插件仍能路由后缀（sidecar 兜底）——
// 这保证用户打开 .zzz 时看到的是「插件不可用」诊断，而不是「未找到插件」。
// ---------------------------------------------------------------------------
static void verifyFailedPluginRouting()
{
    PluginManager *pm = PluginManager::getInstance();
    const QString sPath = pm->pluginForExtension(QStringLiteral("zzz"));
    report(!sPath.isEmpty()
               && QFileInfo(sPath).fileName().compare(QStringLiteral("ZZBroken.dll"), Qt::CaseInsensitive) == 0
               && pm->getInterface(sPath) == nullptr,
           QStringLiteral("W9a: failed plugin still routes .zzz (sidecar) -> '%1'")
               .arg(QFileInfo(sPath).fileName()));

    ProbeMainWindow mw;
    const QString sChosen = mw.choosePlugin(QStringLiteral("zzz"));
    report(sChosen == sPath && !sChosen.isEmpty(),
           QStringLiteral("W9b: choosePlugin returns the unavailable plugin path"));
}

// ---------------------------------------------------------------------------
// Y: C2 数据通道（PluginFile / sendFile）
// ---------------------------------------------------------------------------

// 只实现旧接口（sendFileData）的插件：用来验证 sendFile() 的默认兼容实现
class LegacyOnlyPlugin : public PluginInterface
{
public:
    int apiVersion() const override { return PLUGIN_API_VERSION; }
    void sendFileData(char *data, qint64 len) override
    {
        received = (data != nullptr && len > 0) ? QByteArray(data, qsizetype(len)) : QByteArray();
        ++calls;
    }
    QWidget *getPluginWidget() override { return nullptr; }

    QByteArray received;
    int calls = 0;
};

// 声明了 prefersFileByPath() 的插件：默认 sendFile() 应把路径原样转给它
class PathOnlyPlugin : public PluginInterface
{
public:
    int apiVersion() const override { return PLUGIN_API_VERSION; }
    void sendFileData(char *, qint64) override {}
    QWidget *getPluginWidget() override { return nullptr; }
    bool prefersFileByPath() const override { return true; }
    bool sendFileByPath(const QString &path, const QString &displayName) override
    {
        gotPath = path;
        gotName = displayName;
        return true;
    }

    QString gotPath;
    QString gotName;
};

static void verifyFileChannel(PluginInterface *wordTemplate, const QString &work)
{
    // Y1: PluginFile 内存形态
    const QByteArray payload = QByteArrayLiteral("hello-plugin-file");
    const PluginFile mem = PluginFile::fromMemory(payload, QStringLiteral("demo.bin"));
    report(mem.isMemory() && !mem.isPath() && mem.size() == payload.size()
               && mem.memory() == payload && mem.readAll() == payload
               && mem.displayName() == QStringLiteral("demo.bin"),
           QStringLiteral("Y1: PluginFile memory source"));

    // Y2: PluginFile 路径形态
    QTemporaryDir tmp;
    report(tmp.isValid(), QStringLiteral("Y2a: temp dir for path source"));
    const QString sPayloadFile = tmp.path() + QStringLiteral("/payload.bin");
    {
        QFile f(sPayloadFile);
        f.open(QIODevice::WriteOnly);
        f.write(payload);
    }
    const PluginFile path = PluginFile::fromPath(sPayloadFile, QStringLiteral("payload.bin"));
    report(path.isPath() && !path.isMemory() && path.size() == payload.size()
               && path.readAll() == payload && path.displayName() == QStringLiteral("payload.bin"),
           QStringLiteral("Y2b: PluginFile path source"));

    // Y3: 老插件（只实现 sendFileData）通过默认实现照常收到数据 —— 兼容层关键用例
    LegacyOnlyPlugin legacy;
    report(legacy.sendFile(mem) && legacy.calls == 1 && legacy.received == payload,
           QStringLiteral("Y3a: default sendFile() keeps legacy plugin working (memory)"));
    legacy.calls = 0;
    legacy.received.clear();
    report(legacy.sendFile(path) && legacy.calls == 1 && legacy.received == payload,
           QStringLiteral("Y3b: default sendFile() reads path for legacy plugin"));

    // Y4: 路径型插件：默认实现应把路径原样转给 sendFileByPath()
    PathOnlyPlugin pathOnly;
    report(pathOnly.sendFile(path) && pathOnly.gotPath == sPayloadFile
               && pathOnly.gotName == QStringLiteral("payload.bin"),
           QStringLiteral("Y4: path-capable plugin gets path unchanged"));

    // Y5: 已迁移的插件（PluginWord）直接吃 PluginFile，渲染结果与旧通道一致
    const QByteArray docx = readFile(work + QStringLiteral("/basic.docx"));
    PluginInterface *own = wordTemplate->createInstance();
    own->setSourceName(QStringLiteral("basic.docx"));
    report(own->sendFile(PluginFile::fromMemory(docx, QStringLiteral("basic.docx"))),
           QStringLiteral("Y5a: PluginWord::sendFile(memory) accepted (%1 bytes)").arg(docx.size()));
    QTextDocument *doc = documentOf(own->getPluginWidget());
    report(doc != nullptr && doc->toPlainText().contains(QString::fromUtf8("一级标题")),
           QStringLiteral("Y5b: PluginWord rendered through PluginFile"));
    delete own;
}

// ---------------------------------------------------------------------------
// V: 其余查看器的渲染冒烟（Book / Text / Picture）
//
// 此前这三家只有"能加载 + 后缀路由"级覆盖。抽取 Plugins/common/zipreader 与
// 重构插件代理层都需要它们各自渲染一次做兜底 —— 尤其是 Book 的 EPUB 路径，
// 它是共享 ZipReader 的另一个使用者。
// ---------------------------------------------------------------------------
static int countLabelsWithPixmap(QWidget *root)
{
    int n = 0;
    for (QLabel *pLabel : root->findChildren<QLabel *>())
        if (!pLabel->pixmap().isNull())
            ++n;
    return n;
}

static void verifyOtherViewers(const QString &work)
{
    // 调试开关：FW_VIEWER_ONLY=V1/V2/V3 时只跑其中一组（定位退出期崩溃用）
    const QString sOnly = qEnvironmentVariable("FW_VIEWER_ONLY");
    auto want = [&sOnly](const char *id) {
        return sOnly.isEmpty() || sOnly == QLatin1String(id);
    };

    // 从**本程序自己的 plugins/ 目录**加载（也就是 PluginManager 用的那一份）。
    // 不要再从 -plugins 参数指向的构建目录加载：那是同一 DLL 的另一条路径，
    // Windows 会当成两个模块分别映射，卸载其中一份时会让插件侧遗留对象
    // （QTextBrowser 子类 BookBrowser 等）的虚表悬空 —— 退出期直接崩溃。
    // 渲染用 createInstance() 出来的独立实例，用完显式 delete，模板留给 PluginManager。
    const QString sPluginDir = QCoreApplication::applicationDirPath() + QStringLiteral("/plugins");

    // V1: PluginText —— .txt 原样显示在文本编辑框
    if (want("V1"))
    {
        QPluginLoader loader(sPluginDir + QStringLiteral("/PluginText.dll"));
        PluginInterface *templ = qobject_cast<PluginInterface *>(loader.instance());
        PluginInterface *p = templ ? templ->createInstance() : nullptr;
        bool bOk = false;
        if (p != nullptr)
        {
            const QByteArray data = readFile(work + QStringLiteral("/sample.txt"));
            p->sendFileData(const_cast<char *>(data.constData()), data.size());
            QWidget *w = p->getPluginWidget();
            QTextEdit *pEdit = w ? w->findChild<QTextEdit *>() : nullptr;
            bOk = pEdit != nullptr
                  && pEdit->toPlainText().contains(QString::fromUtf8("文本插件冒烟"));
        }
        delete p;
        report(bOk, QStringLiteral("V1: PluginText renders .txt"));
    }

    // V2: PluginPicture —— .png 变成非空的 QPixmap
    if (want("V2"))
    {
        QPluginLoader loader(sPluginDir + QStringLiteral("/PluginPicture.dll"));
        PluginInterface *templ = qobject_cast<PluginInterface *>(loader.instance());
        PluginInterface *p = templ ? templ->createInstance() : nullptr;
        bool bOk = false;
        if (p != nullptr)
        {
            const QByteArray data = readFile(work + QStringLiteral("/pic.png"));
            p->sendFileData(const_cast<char *>(data.constData()), data.size());
            QWidget *w = p->getPluginWidget();
            bOk = w != nullptr && countLabelsWithPixmap(w) > 0;
        }
        delete p;
        report(bOk, QStringLiteral("V2: PluginPicture renders .png"));
    }

    // V3: PluginBook —— .epub 解析出章节正文（同时覆盖共享 ZipReader 的 deflate 路径）
    if (want("V3"))
    {
        QPluginLoader loader(sPluginDir + QStringLiteral("/PluginBook.dll"));
        PluginInterface *templ = qobject_cast<PluginInterface *>(loader.instance());
        PluginInterface *p = templ ? templ->createInstance() : nullptr;
        bool bOk = false;
        if (p != nullptr)
        {
            const QByteArray data = readFile(work + QStringLiteral("/sample.epub"));
            p->sendFileData(const_cast<char *>(data.constData()), data.size());
            QWidget *w = p->getPluginWidget();
            QTextBrowser *pBrowser = w ? w->findChild<QTextBrowser *>() : nullptr;
            bOk = pBrowser != nullptr
                  && pBrowser->toPlainText().contains(QString::fromUtf8("第一章 冒烟"));
        }
        delete p;
        report(bOk, QStringLiteral("V3: PluginBook renders .epub (shared ZipReader)"));
    }

    // V4: PluginText 对 .html/.htm 走富文本渲染（不是把标签当源码显示）
    if (want("V4"))
    {
        QPluginLoader loader(sPluginDir + QStringLiteral("/PluginText.dll"));
        PluginInterface *templ = qobject_cast<PluginInterface *>(loader.instance());
        PluginInterface *p = templ ? templ->createInstance() : nullptr;
        bool bOk = false;
        if (p != nullptr)
        {
            p->setSourceName(QStringLiteral("sample.html"));
            const QByteArray data = readFile(work + QStringLiteral("/sample.html"));
            p->sendFileData(const_cast<char *>(data.constData()), data.size());
            QWidget *w = p->getPluginWidget();
            QTextEdit *pEdit = w ? w->findChild<QTextEdit *>() : nullptr;
            // 标题被渲染成 heading 块，说明走的是 setHtml 而不是 setPlainText
            bOk = pEdit != nullptr
                  && pEdit->toPlainText().contains(QString::fromUtf8("HTML 标题"))
                  && pEdit->document()->firstBlock().blockFormat().headingLevel() > 0;
        }
        delete p;
        report(bOk, QStringLiteral("V4: PluginText renders .html as rich text"));
    }

    // V5: PluginPicture 借 Qt 的 qsvg 插件解码矢量图
    if (want("V5"))
    {
        QPluginLoader loader(sPluginDir + QStringLiteral("/PluginPicture.dll"));
        PluginInterface *templ = qobject_cast<PluginInterface *>(loader.instance());
        PluginInterface *p = templ ? templ->createInstance() : nullptr;
        bool bOk = false;
        if (p != nullptr)
        {
            p->setSourceName(QStringLiteral("sample.svg"));
            const QByteArray data = readFile(work + QStringLiteral("/sample.svg"));
            p->sendFileData(const_cast<char *>(data.constData()), data.size());
            QWidget *w = p->getPluginWidget();
            bOk = w != nullptr && countLabelsWithPixmap(w) > 0;
        }
        delete p;
        report(bOk, QStringLiteral("V5: PluginPicture renders .svg (qsvg)"));
    }

    // V6: PluginWord 的 ODT（OpenDocument Text）分支：段落/标题 + 表格
    if (want("V6"))
    {
        QPluginLoader loader(sPluginDir + QStringLiteral("/PluginWord.dll"));
        PluginInterface *templ = qobject_cast<PluginInterface *>(loader.instance());
        PluginInterface *p = templ ? templ->createInstance() : nullptr;
        bool bText = false;
        bool bTable = false;
        if (p != nullptr)
        {
            p->setSourceName(QStringLiteral("sample.odt"));
            const QByteArray data = readFile(work + QStringLiteral("/sample.odt"));
            p->sendFileData(const_cast<char *>(data.constData()), data.size());
            QTextDocument *doc = documentOf(p->getPluginWidget());
            if (doc != nullptr)
            {
                const QString plain = doc->toPlainText();
                bText = plain.contains(QString::fromUtf8("第一章 ODT 冒烟"))
                        && plain.contains(QString::fromUtf8("单元格 B2"));
                bTable = doc->toHtml().contains(QStringLiteral("<table"));
            }
        }
        delete p;
        report(bText, QStringLiteral("V6a: PluginWord renders .odt text (ODF)"));
        report(bTable, QStringLiteral("V6b: PluginWord renders .odt table"));
    }

    // V7: PluginPicture 解码 WebP（qwebp 已在部署清单里）
    if (want("V7"))
    {
        QPluginLoader loader(sPluginDir + QStringLiteral("/PluginPicture.dll"));
        PluginInterface *templ = qobject_cast<PluginInterface *>(loader.instance());
        PluginInterface *p = templ ? templ->createInstance() : nullptr;
        bool bOk = false;
        if (p != nullptr)
        {
            p->setSourceName(QStringLiteral("sample.webp"));
            const QByteArray data = readFile(work + QStringLiteral("/sample.webp"));
            p->sendFileData(const_cast<char *>(data.constData()), data.size());
            QWidget *w = p->getPluginWidget();
            bOk = w != nullptr && countLabelsWithPixmap(w) > 0;
        }
        delete p;
        report(bOk, QStringLiteral("V7: PluginPicture renders .webp (qwebp)"));
    }

    // V8: PluginWord 的 RTF 子集分支：段落 + 字符格式（.rtf 不再是"不支持"提示）
    if (want("V8"))
    {
        QPluginLoader loader(sPluginDir + QStringLiteral("/PluginWord.dll"));
        PluginInterface *templ = qobject_cast<PluginInterface *>(loader.instance());
        PluginInterface *p = templ ? templ->createInstance() : nullptr;
        bool bText = false;
        bool bBold = false;
        if (p != nullptr)
        {
            p->setSourceName(QStringLiteral("sample.rtf"));
            const QByteArray data = readFile(work + QStringLiteral("/sample.rtf"));
            p->sendFileData(const_cast<char *>(data.constData()), data.size());
            QTextDocument *doc = documentOf(p->getPluginWidget());
            if (doc != nullptr)
            {
                const QString plain = doc->toPlainText();
                bText = plain.contains(QString::fromUtf8("第一章 RTF 冒烟"))
                        && plain.contains(QString::fromUtf8("粗体"));
                bBold = doc->toHtml().contains(QStringLiteral("font-weight"));
            }
        }
        delete p;
        report(bText, QStringLiteral("V8a: PluginWord renders .rtf text (RTF parser)"));
        report(bBold, QStringLiteral("V8b: PluginWord applies .rtf character formatting"));
    }

    // V9/V10: PluginSheet —— .xlsx（含合并单元格、第二张表）与 .ods
    if (want("V9"))
    {
        QPluginLoader loader(sPluginDir + QStringLiteral("/PluginSheet.dll"));
        PluginInterface *templ = qobject_cast<PluginInterface *>(loader.instance());
        PluginInterface *p = templ ? templ->createInstance() : nullptr;
        bool bOk = false;
        if (p != nullptr)
        {
            p->setSourceName(QStringLiteral("sample.xlsx"));
            const QByteArray data = readFile(work + QStringLiteral("/sample.xlsx"));
            p->sendFileData(const_cast<char *>(data.constData()), data.size());
            QTextDocument *doc = documentOf(p->getPluginWidget());
            if (doc != nullptr)
            {
                const QString plain = doc->toPlainText();
                bOk = plain.contains(QString::fromUtf8("张三"))
                      && plain.contains(QStringLiteral("95"))
                      && plain.contains(QString::fromUtf8("第二张表"))
                      && doc->toHtml().contains(QStringLiteral("<table"));
            }
        }
        delete p;
        report(bOk, QStringLiteral("V9: PluginSheet renders .xlsx (cells + 2 sheets)"));
    }

    if (want("V10"))
    {
        QPluginLoader loader(sPluginDir + QStringLiteral("/PluginSheet.dll"));
        PluginInterface *templ = qobject_cast<PluginInterface *>(loader.instance());
        PluginInterface *p = templ ? templ->createInstance() : nullptr;
        bool bOk = false;
        if (p != nullptr)
        {
            p->setSourceName(QStringLiteral("sample.ods"));
            const QByteArray data = readFile(work + QStringLiteral("/sample.ods"));
            p->sendFileData(const_cast<char *>(data.constData()), data.size());
            QTextDocument *doc = documentOf(p->getPluginWidget());
            if (doc != nullptr)
            {
                const QString plain = doc->toPlainText();
                bOk = plain.contains(QString::fromUtf8("王五"))
                      && plain.contains(QStringLiteral("77"))
                      && doc->toHtml().contains(QStringLiteral("<table"));
            }
        }
        delete p;
        report(bOk, QStringLiteral("V10: PluginSheet renders .ods"));
    }

    // V11/V12: PluginSlides —— .pptx 与 .odp 的逐页静态渲染
    if (want("V11"))
    {
        QPluginLoader loader(sPluginDir + QStringLiteral("/PluginSlides.dll"));
        PluginInterface *templ = qobject_cast<PluginInterface *>(loader.instance());
        PluginInterface *p = templ ? templ->createInstance() : nullptr;
        bool bOk = false;
        if (p != nullptr)
        {
            p->setSourceName(QStringLiteral("sample.pptx"));
            const QByteArray data = readFile(work + QStringLiteral("/sample.pptx"));
            p->sendFileData(const_cast<char *>(data.constData()), data.size());
            QTextDocument *doc = documentOf(p->getPluginWidget());
            if (doc != nullptr)
            {
                const QString plain = doc->toPlainText();
                bOk = plain.contains(QString::fromUtf8("PPTX 冒烟标题"))
                      && plain.contains(QString::fromUtf8("第二页文本框内容"))
                      && plain.contains(QString::fromUtf8("第 2 页"));
            }
        }
        delete p;
        report(bOk, QStringLiteral("V11: PluginSlides renders .pptx (2 slides)"));
    }

    if (want("V12"))
    {
        QPluginLoader loader(sPluginDir + QStringLiteral("/PluginSlides.dll"));
        PluginInterface *templ = qobject_cast<PluginInterface *>(loader.instance());
        PluginInterface *p = templ ? templ->createInstance() : nullptr;
        bool bOk = false;
        if (p != nullptr)
        {
            p->setSourceName(QStringLiteral("sample.odp"));
            const QByteArray data = readFile(work + QStringLiteral("/sample.odp"));
            p->sendFileData(const_cast<char *>(data.constData()), data.size());
            QTextDocument *doc = documentOf(p->getPluginWidget());
            if (doc != nullptr)
            {
                const QString plain = doc->toPlainText();
                bOk = plain.contains(QString::fromUtf8("ODP 冒烟标题"))
                      && plain.contains(QString::fromUtf8("第二页标题"))
                      && plain.contains(QString::fromUtf8("第 2 页"));
            }
        }
        delete p;
        report(bOk, QStringLiteral("V12: PluginSlides renders .odp (2 slides)"));
    }
}

// ---------------------------------------------------------------------------
// Z: 格式识别（FormatResolver）—— 用户遇到"后缀不认识"的文件时，先按内容判断
// ---------------------------------------------------------------------------
static void verifyFormatResolver()
{
    // Z1: 标准 PNG 魔数（8 字节魔数 + 4 字节随便的内容）
    const QByteArray png("\x89PNG\r\n\x1a\nrest", 12);
    const auto d1 = FormatResolver::detect(QStringLiteral("photo.png"), png);
    report(d1.formatId == QLatin1String("png") && d1.pluginExt == QLatin1String("png")
               && d1.fromContent,
           QStringLiteral("F1: content detects PNG (%1)").arg(d1.displayName));

    // Z2: 后缀不可信：扩展名 .zzz 但内容是 PNG -> 仍然交给图片插件
    const auto d2 = FormatResolver::detect(QStringLiteral("photo.zzz"), png);
    report(d2.pluginExt == QLatin1String("png"),
           QStringLiteral("F2: content wins over wrong extension"));

    // Z3: 旧版 Office（OLE2）：识别得出来，但没有插件能处理，必须给可照做的建议
    QByteArray ole("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8);
    ole += QByteArray(64, '\0');
    const auto d3 = FormatResolver::detect(QStringLiteral("old.doc"), ole);
    report(d3.formatId == QLatin1String("ole2") && d3.pluginExt.isEmpty()
               && d3.advice.contains(QString::fromUtf8("另存为")),
           QStringLiteral("F3: legacy OLE2 recognized with actionable advice"));

    // Z4: ZIP 家族：按包内标志区分 docx / xlsx / odt
    QByteArray zipDocx("PK\x03\x04", 4);
    zipDocx += "....word/document.xml....";
    QByteArray zipXlsx("PK\x03\x04", 4);
    zipXlsx += "....xl/workbook.xml....";
    QByteArray zipOdt("PK\x03\x04", 4);
    zipOdt += "...mimetypeapplication/vnd.oasis.opendocument.text...";
    const auto d4 = FormatResolver::detect(QStringLiteral("noext"), zipDocx);
    const auto d5 = FormatResolver::detect(QStringLiteral("noext"), zipXlsx);
    const auto d6 = FormatResolver::detect(QStringLiteral("noext"), zipOdt);
    report(d4.pluginExt == QLatin1String("docx")
               && d5.pluginExt == QLatin1String("xlsx")
               && d6.pluginExt == QLatin1String("odt"),
           QStringLiteral("F4: ZIP subtypes -> docx/xlsx/odt (%1/%2/%3)")
               .arg(d4.pluginExt, d5.pluginExt, d6.pluginExt));

    // Z5: 完全未知：要有可读建议，而不是空
    const auto d7 = FormatResolver::detect(QStringLiteral("blob.qqq"), QByteArray());
    report(d7.formatId == QLatin1String("unknown")
               && d7.displayName.contains(QString::fromUtf8("未知"))
               && !d7.advice.isEmpty(),
           QStringLiteral("F5: unknown format has readable advice"));

    // Z6: 常见不支持格式清单非空，且包含旧版 Office 的处理建议
    const auto unsupported = FormatResolver::knownUnsupported();
    bool bDocAdvice = false;
    for (const auto &p : unsupported)
        if (p.first.contains(QLatin1String(".doc"))) bDocAdvice = true;
    report(!unsupported.isEmpty() && bDocAdvice,
           QStringLiteral("F6: unsupported-format advice list built (%1 entries)")
               .arg(unsupported.size()));
}

static void verifyInstances(PluginInterface *tmpl, const QString &work){
    PluginInterface *a = tmpl->createInstance();
    PluginInterface *b = tmpl->createInstance();
    report(a != nullptr && b != nullptr && a != b, QStringLiteral("M1: two independent instances"));
    if (!a || !b) return;

    QByteArray d1 = readFile(work + QStringLiteral("/basic.docx"));
    a->setSourceName(QStringLiteral("basic.docx"));
    a->sendFileData(d1.data(), d1.size());

    QByteArray d2 = readFile(work + QStringLiteral("/chinese.docx"));
    b->setSourceName(QStringLiteral("chinese.docx"));
    b->sendFileData(d2.data(), d2.size());

    QTextDocument *da = documentOf(a->getPluginWidget());
    QTextDocument *db = documentOf(b->getPluginWidget());
    const QString ta = da ? da->toPlainText() : QString();
    const QString tb = db ? db->toPlainText() : QString();
    report(ta.contains(QString::fromUtf8("一级标题")) && !ta.contains(QString::fromUtf8("中文样式标题")),
           QStringLiteral("M2: instance A shows only its own document"));
    report(tb.contains(QString::fromUtf8("中文样式标题")) && !tb.contains(QString::fromUtf8("一级标题")),
           QStringLiteral("M3: instance B shows only its own document"));

    delete b;
    const QString ta2 = da ? da->toPlainText() : QString();
    report(ta2.contains(QString::fromUtf8("一级标题")), QStringLiteral("M4: A survives after B destroyed"));
    delete a;
}

// ---------------------------------------------------------------------------
// A: 主程序链路（PluginManager 加载 / 合并信号 / resolvePlugin 三步 / 标签页挂载）
// ---------------------------------------------------------------------------
class IntegrationWindow : public MainWindow
{
public:
    IntegrationWindow() : MainWindow(nullptr) {}
    using MainWindow::choosePlugin;    // protected -> public
};

// ---------------------------------------------------------------------------
// X: B2（实例创建不再退化）/ B4（磁盘空间预检）/ C5（插件启停 + 重新加载）
// 放在最后：C5 的 reloadPlugins() 会卸载插件 DLL，之前创建的实例必须都已销毁。
// ---------------------------------------------------------------------------

// 一个"没有实现 createInstance()"的插件替身（用接口默认实现 -> nullptr）
class NullInstancePlugin : public PluginInterface
{
public:
    int apiVersion() const override { return PLUGIN_API_VERSION; }
    void sendFileData(char *, qint64) override {}
    QWidget *getPluginWidget() override { return nullptr; }
};

static void verifyCreateInstanceContract(PluginInterface *realTemplate)
{
    // X1: 正常插件：必须能创建出与模板不同的独立实例
    QString sErr;
    PluginInterface *pNew = PluginManager::createInstanceFor(realTemplate, &sErr);
    report(pNew != nullptr && pNew != realTemplate && sErr.isEmpty(),
           QStringLiteral("X1: real plugin creates a distinct per-tab instance"));
    delete pNew;

    // X2: 返回 nullptr 的插件 → 明确失败 + 可读原因（以前会静默退化成共享实例，
    //     导致多个标签页互相覆盖内容）
    NullInstancePlugin stub;
    sErr.clear();
    PluginInterface *pNull = PluginManager::createInstanceFor(&stub, &sErr);
    report(pNull == nullptr && !sErr.isEmpty(),
           QStringLiteral("X2: createInstance()==nullptr rejected ('%1')").arg(sErr.left(36)));
}

static void verifyDiskSpaceCheck()
{
    qint64 nAvail = -1;
    const QString sTemp = QDir::tempPath();

    // X3a: 要 1 字节：只要临时盘可用就应通过，并能报出可用空间
    report(FileManager::hasFreeSpaceFor(sTemp, 1, &nAvail) && nAvail > 0,
           QStringLiteral("X3a: free space probe works (%1 MB available)")
               .arg(nAvail / (1024.0 * 1024.0), 0, 'f', 0));

    // X3b: 要 2^62 字节（任何磁盘都放不下）→ 必须拒绝（这条能真正守住"几 GB 条目
    //      在空间不足时不要退回内存通道"的逻辑）
    report(!FileManager::hasFreeSpaceFor(sTemp, (1LL << 62)),
           QStringLiteral("X3b: absurd requirement rejected"));
}

static void verifyPluginToggleAndReload()
{
    PluginManager *pm = PluginManager::getInstance();
    const QString sPdfPath = QCoreApplication::applicationDirPath()
                             + QStringLiteral("/plugins/PluginPdf.dll");
    const QString sPdfId = QStringLiteral("PluginPdf");

    const bool bWasEnabled = PluginManager::isPluginEnabled(sPdfId);

    // X4: 禁用 → 重新加载：不再加载，但后缀仍然路由到它（这样打开 .pdf 会提示
    //     "插件已被禁用" 而不是 "未找到插件"）
    PluginManager::setPluginEnabled(sPdfId, false);
    pm->reloadPlugins();
    report(!PluginManager::isPluginEnabled(sPdfId)
               && pm->getInterface(sPdfPath) == nullptr
               && !pm->pluginForExtension(QStringLiteral("pdf")).isEmpty()
               && pm->disabledPluginIds().contains(sPdfId),
           QStringLiteral("X4: disabled plugin not loaded, .pdf still routed, listed as disabled"));

    // X5: 重新启用 → 重新加载：恢复
    PluginManager::setPluginEnabled(sPdfId, true);
    pm->reloadPlugins();
    report(PluginManager::isPluginEnabled(sPdfId)
               && pm->getInterface(sPdfPath) != nullptr
               && pm->failures().isEmpty(),
           QStringLiteral("X5: re-enabled plugin loads again (%1 plugins)")
               .arg(pm->getAllInterfaces().size()));

    // 恢复测试前的设置，避免影响用户配置
    if (!bWasEnabled)
        PluginManager::setPluginEnabled(sPdfId, false);

    // X6: 已加载失败的插件被用户禁用后，「重试加载」不应把它复活，失败记录也应清掉
    //     （复核时修的 bug：retryFailedPlugins 没看禁用状态）
    const QString sScanDir = QCoreApplication::applicationDirPath() + QStringLiteral("/plugins");
    const QString sBadDll  = sScanDir + QStringLiteral("/ZZDisabled.dll");
    const QString sBadJson = sScanDir + QStringLiteral("/ZZDisabled.json");
    QFile::remove(sBadDll);
    QFile::remove(sBadJson);
    {
        QFile f(sBadDll);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write("not a plugin");
    }
    {
        QFile f(sBadJson);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write("{\"Name\":\"zzDisabled\",\"apiVersion\":3,\"extensions\":[\"zzd\"]}");
    }

    pm->reloadPlugins();                       // 这次扫描会把 ZZDisabled 记为加载失败
    const bool bRecorded = !pm->failureFor(sBadDll).isEmpty();
    PluginManager::setPluginEnabled(QStringLiteral("ZZDisabled"), false);
    const int nRetried = pm->retryFailedPlugins();
    const bool bStillFailed = !pm->failureFor(sBadDll).isEmpty();
    report(bRecorded && nRetried == 0 && !bStillFailed,
           QStringLiteral("X6: disabled plugin not resurrected by retry (recorded=%1 retried=%2)")
               .arg(bRecorded).arg(nRetried));

    // 收尾：恢复启用、清掉测试文件
    PluginManager::setPluginEnabled(QStringLiteral("ZZDisabled"), true);
    QFile::remove(sBadDll);
    QFile::remove(sBadJson);
}

// ---------------------------------------------------------------------------
// G: 合并时"已合并的 .dat"先拆分成条目，而不是当普通文件嵌套进去
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// H: 合并对话框的交互契约（多选 + 删除选中项）
// ---------------------------------------------------------------------------
static void verifyMergeDialog()
{
    DlgMergeFile dlg;

    QTreeView *pTree = dlg.findChild<QTreeView *>();
    report(pTree != nullptr
               && pTree->selectionMode() == QAbstractItemView::ExtendedSelection
               && pTree->selectionBehavior() == QAbstractItemView::SelectRows,
           QStringLiteral("H1: merge dialog list supports multi-select rows"));

    QStandardItemModel *pModel =
        pTree ? qobject_cast<QStandardItemModel *>(pTree->model()) : nullptr;
    report(pModel != nullptr, QStringLiteral("H2: path list uses a standard item model"));

    if (pModel != nullptr && pTree != nullptr)
    {
        // 模拟「添加文件」后的列表：3 项
        for (const QString &sName : { QStringLiteral("a.txt"), QStringLiteral("b.txt"),
                                      QStringLiteral("c.txt") })
            pModel->setItem(pModel->rowCount(), 0, new QStandardItem(sName));

        // 选中第 0、2 行，点「删除」→ 只剩 b.txt
        QItemSelectionModel *pSel = pTree->selectionModel();
        pSel->select(pModel->index(0, 0),
                     QItemSelectionModel::Select | QItemSelectionModel::Rows);
        pSel->select(pModel->index(2, 0),
                     QItemSelectionModel::Select | QItemSelectionModel::Rows);

        QPushButton *pDelete = dlg.findChild<QPushButton *>(QStringLiteral("btnDelete"));
        if (pDelete) pDelete->click();

        report(pModel->rowCount() == 1
                   && pModel->item(0, 0) && pModel->item(0, 0)->text() == QStringLiteral("b.txt"),
               QStringLiteral("H3: delete removes only the selected rows (%1 left)")
                   .arg(pModel->rowCount()));
    }

    // 复选框去掉后，Delete 键作为等价快捷方式应存在
    bool bHasDeleteShortcut = false;
    for (QShortcut *pSc : dlg.findChildren<QShortcut *>())
        if (pSc->key() == QKeySequence(QKeySequence::Delete)) bHasDeleteShortcut = true;
    report(bHasDeleteShortcut, QStringLiteral("H4: Delete key shortcut wired to delete"));
}

static void verifyMergeFlattening(const QString &work, const QString &scratchDir)
{
    auto hasSuffix = [](const QStringList &paths, const QString &suffix) {
        for (const QString &p : paths)
            if (p.endsWith(suffix)) return true;
        return false;
    };
    auto listPaths = [](const FwArc::Meta &m) {
        QStringList l;
        for (const FwArc::Entry &e : m.entries) l << e.relPath;
        return l;
    };

    const QString sArcInner = scratchDir + QStringLiteral("/merge_inner.dat");
    const QString sArcOuter = scratchDir + QStringLiteral("/merge_outer.dat");
    const QString sArcDup   = scratchDir + QStringLiteral("/merge_dup.dat");
    const QString sArcRoot  = scratchDir + QStringLiteral("/merge_rooted.dat");
    const QString sNote     = scratchDir + QStringLiteral("/merge_note.txt");
    {
        QFile f(sNote);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write("merge note");
    }

    // G1: 同一目录下普通合并 -> 存储路径是短相对名（root = 素材目录）
    FileManager fm1;
    fm1.QMergeFiles(QVector<QString>{ work + QStringLiteral("/basic.docx"),
                                      work + QStringLiteral("/pic.png") }, sArcInner);
    FwArc::Meta inner;
    QString err1;
    const bool bMeta1 = FileManager::inspectArchive(sArcInner, inner, err1);
    const QStringList innerPaths = listPaths(inner);
    const bool bG1 = bMeta1
                     && innerPaths.size() == 2
                     && innerPaths.contains(QStringLiteral("basic.docx"))
                     && innerPaths.contains(QStringLiteral("pic.png"));
    report(bG1, QStringLiteral("G1: same-dir merge keeps short relative names (%1)")
                    .arg(innerPaths.join(QStringLiteral(", "))));

    // G2: "已有归档 + 普通文件"再合并 -> 归档被拆开，两个来源都不丢，且没有嵌套 .dat
    //     （临时目录与素材目录可能不同盘，走扁平模式；这里只断言"来源路径结尾"）
    FileManager fm2;
    fm2.QMergeFiles(QVector<QString>{ sArcInner, sNote }, sArcOuter);
    FwArc::Meta outer;
    QString err2;
    const bool bG2 = FileManager::inspectArchive(sArcOuter, outer, err2);
    const QStringList outerPaths = listPaths(outer);
    const bool bNoNested = !hasSuffix(outerPaths, QStringLiteral(".dat"));
    report(bG2 && outerPaths.size() == 3 && bNoNested
               && hasSuffix(outerPaths, QStringLiteral("basic.docx"))
               && hasSuffix(outerPaths, QStringLiteral("pic.png"))
               && hasSuffix(outerPaths, QStringLiteral("merge_note.txt")),
           QStringLiteral("G2: nested archive flattened, both origins kept (%1)")
               .arg(outerPaths.join(QStringLiteral(", "))));

    // G3: 路径撞名自动改名（归档里已有 pic.png，同时又选了 pic.png）
    FileManager fm3;
    fm3.QMergeFiles(QVector<QString>{ sArcInner, work + QStringLiteral("/pic.png") }, sArcDup);
    FwArc::Meta dup;
    QString err3;
    const bool bG3 = FileManager::inspectArchive(sArcDup, dup, err3);
    const QStringList dupPaths = listPaths(dup);
    report(bG3 && dupPaths.size() == 3
               && hasSuffix(dupPaths, QStringLiteral("pic.png"))
               && hasSuffix(dupPaths, QStringLiteral("pic (2).png")),
           QStringLiteral("G3: duplicate path auto-renamed (%1)")
               .arg(dupPaths.join(QStringLiteral(", "))));

    // G4: 来源归档的 root 与当前盘符不同（rooted.dat 的 root = D:/testfiles_copy）：
    //     展开后必须保留它原来的归属，不能被改挂到另一个盘下（用户实际报过的问题）
    FileManager fm4;
    fm4.QMergeFiles(QVector<QString>{ work + QStringLiteral("/rooted.dat"),
                                      work + QStringLiteral("/basic.docx") }, sArcRoot);
    FwArc::Meta rooted;
    QString err4;
    const bool bG4 = FileManager::inspectArchive(sArcRoot, rooted, err4);
    const QStringList rootedPaths = listPaths(rooted);
    report(bG4 && rooted.root.isEmpty()
               && rootedPaths.contains(QStringLiteral("D/testfiles_copy/a/file1.txt"))
               && hasSuffix(rootedPaths, QStringLiteral("basic.docx")),
           QStringLiteral("G4: archived origin preserved cross-drive (%1)")
               .arg(rootedPaths.join(QStringLiteral(", "))));
}

static void verifyAppIntegration(const QString &work, const QString &scratchDir)
{
    IntegrationWindow mw;

    // A1: 主程序启动时就是这样加载插件的（目录 = exe 同级 plugins/）
    PluginManager::getInstance()->loadAllPlugins();
    const int loaded = PluginManager::getInstance()->getAllInterfaces().size();
    const QString wordDll = QCoreApplication::applicationDirPath()
                            + QStringLiteral("/plugins/PluginWord.dll");
    PluginInterface *tmpl = PluginManager::getInstance()->getInterface(wordDll);
    report(loaded >= 6 && tmpl != nullptr,
           QStringLiteral("A1: PluginManager loaded %1 plugins, PluginWord included").arg(loaded));

    // A2: 主程序的合并信号链路
    const QString dat = scratchDir + QStringLiteral("/appflow.dat");
    QFile::remove(dat);
    const QVector<QString> files = { work + QStringLiteral("/basic.docx"),
                                     work + QStringLiteral("/chinese.docx") };
    const bool invoked = QMetaObject::invokeMethod(&mw, "QMergeFiles",
                          Q_ARG(QVector<QString>, files), Q_ARG(QString, dat));
    report(invoked && QFileInfo(dat).size() > 0,
           QStringLiteral("A2: app merge signal produced %1 bytes").arg(QFileInfo(dat).size()));

    // A3: 与 resolvePlugin 完全相同的三步
    const QString path = mw.choosePlugin(QStringLiteral("docx"));
    PluginInterface *resolved = PluginManager::getInstance()->getInterface(path);
    report(resolved != nullptr && resolved == tmpl,
           QStringLiteral("A3: choosePlugin + getInterface resolved PluginWord"));

    PluginInterface *own = resolved ? resolved->createInstance() : nullptr;
    report(own != nullptr && own != resolved,
           QStringLiteral("A4: createInstance gave a per-tab instance"));
    if (!own) return;

    // A5: 挂进标签页（等价于 activateTab 的挂载），再送数据（等价于 renderFile）
    own->setSourceName(QStringLiteral("basic.docx"));
    QTabWidget tabs;
    tabs.resize(900, 720);
    tabs.addTab(own->getPluginWidget(), QStringLiteral("basic.docx"));
    tabs.show();

    QByteArray data = readFile(work + QStringLiteral("/basic.docx"));
    own->sendFileData(data.data(), data.size());
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 400) QApplication::processEvents();

    QWidget *page = tabs.currentWidget();
    QTextBrowser *browser = page ? page->findChild<QTextBrowser *>() : nullptr;
    const QString text = browser ? browser->toPlainText() : QString();
    report(text.contains(QString::fromUtf8("一级标题")) && text.contains(QString::fromUtf8("合并表头")),
           QStringLiteral("A5: renders in a tab page (%1 chars)").arg(text.size()));
    report(tabs.tabText(0) == QStringLiteral("basic.docx"),
           QStringLiteral("A6: tab title '%1'").arg(tabs.tabText(0)));

    const QPixmap shot = tabs.grab();
    report(!shot.isNull() && shot.width() > 100,
           QStringLiteral("A7: tab page grabs %1x%2 (layout ready)")
               .arg(shot.width()).arg(shot.height()));

    tabs.removeTab(0);
    delete own;

    // A8: 「插件设置 → 插件诊断」动作确实被创建（A2 的用户入口）
    bool bDiagAction = false;
    for (QAction *a : mw.findChildren<QAction *>())
        if (a->text().contains(QStringLiteral("插件诊断"))) { bDiagAction = true; break; }
    report(bDiagAction, QStringLiteral("A8: menu action 插件诊断 exists"));

    // A9: C5 的两个入口（插件管理 / 重新加载插件）也已挂到菜单上
    QStringList sActions;
    for (QAction *a : mw.findChildren<QAction *>())
        sActions << a->text();
    report(sActions.contains(QStringLiteral("插件管理"))
               && sActions.contains(QStringLiteral("重新加载插件")),
           QStringLiteral("A9: menu actions 插件管理 / 重新加载插件 exist"));

    // A10: 「支持的格式」入口（用户遇到不支持的格式时最直接的求助入口）
    report(sActions.contains(QStringLiteral("支持的格式…")),
           QStringLiteral("A10: menu action 支持的格式 exists"));
}

// ---------------------------------------------------------------------------
int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    // 测试必须与用户真实配置隔离：插件映射（QSettings PluginMap）与插件启停
    // （PluginEnabled）都会读写 ini，若用默认路径，一次测试就会改动用户设置。
    // 这里把 UserScope 的 ini 目录指到临时目录（必须在任何 QSettings 构造之前调用）。
    QTemporaryDir settingsDir;
    if (settingsDir.isValid())
    {
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        note(QStringLiteral("settings isolated to: ") + settingsDir.path());
    }

    const QString work = (argc > 1) ? QString::fromLocal8Bit(argv[1])
                                    : QDir::currentPath() + QStringLiteral("/fixtures");
    const QString plug = (argc > 2) ? QString::fromLocal8Bit(argv[2])
                                    : QDir::currentPath() + QStringLiteral("/plugins");
    const QString logPath = (argc > 3) ? QString::fromLocal8Bit(argv[3])
                                       : QDir::currentPath() + QStringLiteral("/plugin_tests.log");

    // 可选开关：--shots <dir> / --integrate
    // 用显式开关而不是位置参数 —— 以前"第 4 个参数是截图目录、第 5 个是 integrate"，
    // 一旦跳过截图，integrate 就会被当成目录名，A 组静默不跑（踩过）。
    QString shotsDir;
    bool integrateMode = false;
    for (int i = 4; i < argc; ++i)
    {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QLatin1String("--integrate"))
        {
            integrateMode = true;
        }
        else if (a == QLatin1String("--shots"))
        {
            if (i + 1 < argc) shotsDir = QString::fromLocal8Bit(argv[++i]);
            else note(QStringLiteral("--shots needs a directory argument, ignored"));
        }
        else
        {
            note(QStringLiteral("ignoring unknown argument: ") + a);
        }
    }

    note(QStringLiteral("=== plugin tests (infrastructure + PluginWord) ==="));
    note(QStringLiteral("fixtures: ") + work);
    note(QStringLiteral("plugins : ") + plug);
    note(QStringLiteral("integrate: ") + (integrateMode ? QStringLiteral("on") : QStringLiteral("off")));

    // 派生文件（合并出的 .dat 等）放临时目录，避免污染入库的 fixtures/
    QTemporaryDir scratch;
    const QString scratchDir = scratch.isValid() ? scratch.path()
                                                 : QDir::tempPath() + QStringLiteral("/plugin_tests");
    if (!scratch.isValid())
        QDir().mkpath(scratchDir);

    // ---------- W1 ----------
    // 先放一个坏插件：MainWindow 构造时（checkExtensionMapping 里）会扫描 plugins/，
    // D5-D7 要靠这次扫描验证「加载失败被记录 + 重试能恢复」。
    installBrokenPluginForScan();
    checkExtensionMapping();

    // ---------- W2-W9: C1 插件自描述后缀 / B1 版本协商 / sidecar 兜底 ----------
    verifyExtensionRegistry(plug);
    verifyApiVersionGate();
    verifyFailedPluginRouting();

    // ---------- W10-W14: 真实归档往返 ----------
    // 派生文件（合并出来的 .dat）写到临时目录：fixtures 目录是入库的素材，
    // 不能被测试产物污染（以前放在 work/ 里所以没暴露这个问题）。
    const QString datPath = scratchDir + QStringLiteral("/word.dat");
    QFile::remove(datPath);
    {
        FileManager fm;
        bool ok = false;
        QObject::connect(&fm, &FileManager::operationFinished,
                         [&ok](bool o, const QString &) { ok = o; });
        fm.QMergeFiles(QVector<QString>() << work + QStringLiteral("/basic.docx")
                                          << work + QStringLiteral("/chinese.docx"),
                       datPath);
        report(ok && QFileInfo(datPath).size() > 0,
               QStringLiteral("W10: merged 2 docx into archive (%1 bytes)").arg(QFileInfo(datPath).size()));
        fm.QLoadMergedFile(datPath);
        report(fm.isArchiveLoaded(), QStringLiteral("W11: archive loaded"));
    }

    FileManager fm;
    fm.QLoadMergedFile(datPath);
    QString basicEntry, chineseEntry;
    for (const FILEINFO &fi : fm.getFileInfos())
    {
        if (fi.sFileName.endsWith(QStringLiteral("basic.docx"), Qt::CaseInsensitive))
            basicEntry = fi.sFilePath;
        else if (fi.sFileName.endsWith(QStringLiteral("chinese.docx"), Qt::CaseInsensitive))
            chineseEntry = fi.sFilePath;
    }

    char *buf = nullptr;
    qint64 len = 0;
    fm.QGetFileContent(basicEntry, &buf, len);
    const qint64 expectLen = QFileInfo(work + QStringLiteral("/basic.docx")).size();
    report(buf != nullptr && len == expectLen,
           QStringLiteral("W12: read back %1 bytes (file %2)").arg(len).arg(expectLen));
    if (!buf) return 1;

    // ---------- W5/W6: 真实插件 DLL ----------
    QPluginLoader loader(plug + QStringLiteral("/PluginWord.dll"));
    QObject *plugin = loader.instance();
    report(plugin != nullptr,
           QStringLiteral("W13: load PluginWord.dll%1")
               .arg(plugin ? QString() : QStringLiteral(" FAILED: ") + loader.errorString()));
    if (!plugin) return 1;
    PluginInterface *tmpl = qobject_cast<PluginInterface *>(plugin);
    report(tmpl != nullptr, QStringLiteral("W14: cast to PluginInterface"));
    if (!tmpl) return 1;

    // ---------- B / Z ----------
    {
        PluginInterface *own = tmpl->createInstance();
        own->setSourceName(QStringLiteral("basic.docx"));
        own->sendFileData(buf, len);
        verifyBasicDoc(own->getPluginWidget());
        verifyZoom(own->getPluginWidget());
        delete own;
    }
    delete[] buf;
    buf = nullptr;

    // ---------- C ----------
    {
        char *cbuf = nullptr;
        qint64 clen = 0;
        fm.QGetFileContent(chineseEntry, &cbuf, clen);
        PluginInterface *own = tmpl->createInstance();
        own->setSourceName(QStringLiteral("chinese.docx"));
        own->sendFileData(cbuf, clen);
        verifyChineseDoc(own->getPluginWidget());
        delete own;
        delete[] cbuf;
    }

    // ---------- N / M ----------
    verifyErrorPaths(tmpl, work);
    verifyInstances(tmpl, work);

    // ---------- Y: C2 数据通道 ----------
    verifyFileChannel(tmpl, work);

    // ---------- V: 其余查看器渲染冒烟 ----------
    if (qEnvironmentVariableIsEmpty("FW_SKIP_VIEWER_SMOKE"))
        verifyOtherViewers(work);

    // ---------- Z: 格式识别（后缀不可靠时的兜底） ----------
    verifyFormatResolver();

    // ---------- S: 截图（可选，--shots <dir>） ----------
    if (!shotsDir.isEmpty())
    {
        const QString shots = shotsDir;
        const struct { const char *file; const char *label; } kShots[] = {
            { "basic.docx",   "basic" },
            { "chinese.docx", "chinese" },
            { "legacy.doc",   "legacy_doc" },
        };
        for (const auto &s : kShots)
        {
            const QByteArray data = readFile(work + QLatin1Char('/') + QLatin1String(s.file));
            PluginInterface *p = tmpl->createInstance();
            p->setSourceName(QString::fromLatin1(s.file));
            QByteArray copy = data;
            p->sendFileData(copy.isEmpty() ? nullptr : copy.data(), copy.size());
            saveShot(p->getPluginWidget(),
                     shots + QLatin1Char('/') + QLatin1String(s.label) + QStringLiteral(".png"));
            delete p;
        }
    }

    // ---------- A: 主程序链路（可选） ----------
    if (integrateMode)
        verifyAppIntegration(work, scratchDir);

    // ---------- G: 合并时展开已合并的归档 ----------
    verifyMergeFlattening(work, scratchDir);

    // ---------- H: 合并对话框交互 ----------
    verifyMergeDialog();

    // ---------- D: 部署自检 + 插件失败可见性 ----------
    // 放在 A 之后：D6「补文件后重试恢复」会额外加载一份插件副本，从而在
    // 后缀注册表里制造冲突告警；放到最后就不会影响前面的任何断言。
    // （D5 需要的"首次扫描记录失败"已在 W1 构造 MainWindow 时发生。）
    verifyDeploymentDiagnostics(plug);

    // ---------- X: B2 / B4 / C5（最后执行：C5 会卸载插件 DLL） ----------
    verifyCreateInstanceContract(tmpl);
    verifyDiskSpaceCheck();
    verifyPluginToggleAndReload();

    // ---------- 汇总 ----------
    out(QStringLiteral("---------------------------------------------"));
    out(QStringLiteral("RESULT: %1 PASS, %2 FAIL").arg(g_pass).arg(g_fail));

    QFile logFile(logPath);
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        logFile.write(g_log.join(QLatin1Char('\n')).toUtf8());
        logFile.write("\n");
    }
    return g_fail == 0 ? 0 : 1;
}

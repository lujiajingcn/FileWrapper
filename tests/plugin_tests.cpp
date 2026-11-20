// ============================================================================
// plugin_tests：插件基础设施 + PluginWord 的端到端验证（真实链路，不掺假）
//
// 覆盖的行为（断言标签前缀）：
//   W   后缀路由：插件自描述 extensions / 未知后缀 / 内容优先仲裁（C1 + 自动路由）
//   W8  API 版本门禁（B1）：当前版本接受、旧插件拒绝、版本不符拒绝、空元数据拒绝
//   W9  加载失败的插件仍能路由后缀（sidecar 兜底，走"插件不可用"诊断）
//   W1b 启动时对加载失败的插件给出可见提示（A2）
//   B/C 真实 docx 渲染：段落/标题/字符格式/对齐/列表/表格合并/图片/超链接/中文编号
//   Z   缩放（缩小/放大/重置）
//   N   异常输入（旧版 .doc / rtf / 损坏 zip / 非 Word 压缩包 / 0 字节）+ 失败清屏
//   M   多标签页实例独立性
//   D   部署自检（清单比对、报告文本、失败登记、重试恢复、真实构建目录自检）
//   A   主程序链路：PluginManager 加载、QMergeFiles 合并、resolvePlugin 三步、
//       插件窗口挂进 QTabWidget、「插件设置」入口存在
//
//   * 真实 FileManager 把真实 .docx 合并成 .dat 归档，再把字节读回来
//     （与程序里双击文件时的取数据路径一致）
//   * 真实插件 DLL，经 QPluginLoader 加载后走 PluginInterface
//   * 真实 QTextBrowser 文档：逐段/逐字符/逐表格/逐图片检查渲染结果
//
// 用法：plugin_tests <fixture-dir> <plugins-dir> <log-file> [--shots <dir>] [--integrate]
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
#include <QDateTime>
#include <QDirIterator>
#include <QEventLoop>
#include <QProcess>
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
#include <QListView>
#include <QStackedWidget>
#include <QHeaderView>
#include <QMouseEvent>
#include <QPushButton>
#include <QLineEdit>
#include <QRadioButton>
#include "dlgoutputfile.h"
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
#include <QSlider>
#include <cstring>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextImageFormat>
#include <QTextTable>
#include <QUrl>

#include <cstdio>
#include <functional>

#include "dlgfileproperty.h"
#include "dlgentrytags.h"
#include "filemanager.h"
#include "filemetadata.h"
#include "dlgmergefile.h"
#include "dlgsplitfile.h"
#include "filelistmodel.h"
#include "formatresolver.h"
#include "mainwindow.h"
#include "plugininterface.h"
#include "pluginmanager.h"
#include "plugindiag.h"
#include "zipreader.h"
#include "xmlguard.h"

static int g_pass = 0;
static int g_fail = 0;
static int g_skipped = 0;   // 环境变量显式跳过的断言组（仍需在汇总中可见）
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

// 受限环境（任务沙箱/AppContainer）下 QTemporaryDir 会把新目录的 ACL 收成
// owner-only，宿主进程本身反而写不进去。优先使用运行器预创建、可写的
// FW_TEST_SCRATCH_DIR；没有该环境变量时退回系统临时目录。
static QString makeWritableTestDir(const QString &prefix)
{
    QString base = qEnvironmentVariable("FW_TEST_SCRATCH_DIR");
    if (base.isEmpty())
        base = QDir::tempPath();
    const QString dir = base + QLatin1Char('/') + prefix + QLatin1Char('-')
                        + QString::number(QCoreApplication::applicationPid())
                        + QLatin1Char('-')
                        + QString::number(QDateTime::currentMSecsSinceEpoch());
    QDir().mkpath(dir);
    return dir;
}

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
    using RouteDecision = MainWindow::RouteDecision;
    using MainWindow::decideRoute;
};

// 树形列表测试：直接加载归档（loadArchiveFile 为 protected）
class ProbeTreeWindow : public MainWindow
{
public:
    ProbeTreeWindow() : MainWindow(nullptr) {}
    using MainWindow::loadArchiveFile;
    using MainWindow::buildFileListContextMenu;
    using MainWindow::filePropertyData;
    using MainWindow::folderPropertyData;
    using MainWindow::renderEntryThumbnail;
    using MainWindow::isThumbnailImageName;
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
        { "tiff", "PluginPicture" }, { "tif", "PluginPicture" },
        { "jpe",  "PluginPicture" }, { "jfif", "PluginPicture" },
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

    // W1b: ".ts" 是唯一跨插件重名的后缀（MPEG-TS 视频 vs TypeScript 文本）：
    //      静态优先级给文本，真视频由内容识别改投媒体插件。
    PluginManager *pm1 = PluginManager::getInstance();

    QByteArray tsHead;
    for (int i = 0; i < 6; ++i)
    {
        tsHead += char(0x47);
        tsHead += QByteArray("\x00\x00\x10", 3);
        tsHead += QByteArray(184, '\x00');
    }
    const QString sTextDll = pm1->pluginForExtension(QStringLiteral("ts"));
    const ProbeMainWindow::RouteDecision tsVideo =
        ProbeMainWindow::decideRoute(QStringLiteral("movie.ts"), tsHead, sTextDll);
    report(QFileInfo(sTextDll).fileName().compare(QStringLiteral("PluginText.dll"), Qt::CaseInsensitive) == 0
               && QFileInfo(tsVideo.pluginPath).fileName().compare(QStringLiteral("PluginMedia.dll"),
                                                          Qt::CaseInsensitive) == 0
               && tsVideo.fromContent && !tsVideo.basis.isEmpty(),
           QStringLiteral("W1d: content-first .ts video -> %1 (%2)")
               .arg(QFileInfo(tsVideo.pluginPath).fileName(), tsVideo.basis));

    const ProbeMainWindow::RouteDecision tsSource =
        ProbeMainWindow::decideRoute(
            QStringLiteral("code.ts"),
            QStringLiteral("import { a } from './b';\nexport const c = a + 1;\n").toUtf8(),
            sTextDll);
    report(QFileInfo(tsSource.pluginPath).fileName().compare(QStringLiteral("PluginText.dll"),
                                                             Qt::CaseInsensitive) == 0
               && !tsSource.fromContent,
           QStringLiteral("W1e: TypeScript source keeps its extension routing (no override)"));

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
    const QString appDir = makeWritableTestDir(QStringLiteral("diag_app"));
    report(QDir(appDir).exists(), QStringLiteral("D1: temp app dir"));
    if (!QDir(appDir).exists()) return;
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
    const QString noManifestDir = makeWritableTestDir(QStringLiteral("diag_nomanifest"));
    QString sNoManifestErr;
    PluginDiagnostics::missingFiles(noManifestDir, &sNoManifestErr);
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
    // 放到独立子进程：PluginPdf 副本一旦在本进程加载，Windows 会一直映射
    // ZZBroken.dll，QFile::remove 永远失败并留下第 9 个模块。子进程退出后
    // 映射自然释放，父进程再清理文件。
    const QString sPath = brokenProbePluginPath();
    QProcess d6Child;
    d6Child.start(QCoreApplication::applicationFilePath(),
                  QStringList{QStringLiteral("--d6-child")});
    const bool bChildStarted = d6Child.waitForStarted(10000);
    if (!bChildStarted)
    {
        // 受限沙箱不允许子进程通过管道启动：这不是产品缺陷，按环境跳过。
        ++g_skipped;
        note(QStringLiteral("[SKIP] D6: cannot start child process in this environment (")
             + d6Child.errorString() + QLatin1Char(')'));
        QFile::remove(sPath.left(sPath.size() - 4) + QStringLiteral(".json"));
    }
    else
    {
    bool bChildFinished = d6Child.waitForFinished(60000);
    if (!bChildFinished)
    {
        d6Child.kill();
        bChildFinished = d6Child.waitForFinished(5000);
    }
    const bool bD6 = bChildFinished && d6Child.exitStatus() == QProcess::NormalExit
                     && d6Child.exitCode() == 0;
    const QString sJson = sPath.left(sPath.size() - 4) + QStringLiteral(".json");
    const bool bDllRemoved = QFile::remove(sPath) || !QFile::exists(sPath);
    const bool bJsonRemoved = QFile::remove(sJson) || !QFile::exists(sJson);
    report(bD6 && bDllRemoved && bJsonRemoved,
           QStringLiteral("D6: retry recovered in isolated process (exit %1, cleanup=%2/%3)")
               .arg(bChildFinished ? d6Child.exitCode() : -999)
               .arg(bDllRemoved).arg(bJsonRemoved));
    if (!bD6)
        note(QStringLiteral("D6 child stderr: ")
             + QString::fromLocal8Bit(d6Child.readAllStandardError()).left(400));
    }

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
    // 因此真实插件唯一路由数是 114（zzz 为 W1 假插件 sidecar），冲突按 extensionPriority 决出。
    // 114 里混有 W1 扫描用的 ZZBroken sidecar 假后缀 zzz；扣除后再核对
    // 真实 8 个插件 JSON 的 114 个唯一后缀（2026-11-20 图片插件补了 tif/jpe/jfif），
    // 避免断言依赖假插件状态。
    const int nRegistered = pm->registeredExtensionCount();
    const int nFake = pm->pluginForExtension(QStringLiteral("zzz")).isEmpty() ? 0 : 1;
    report(nRegistered - nFake == 114,
           QStringLiteral("W4: %1 real unique extensions (fake zzz excluded; raw=%2)")
               .arg(nRegistered - nFake).arg(nRegistered));
    report(QFileInfo(pm->pluginForExtension(QStringLiteral("ts"))).fileName()
               .compare(QStringLiteral("PluginText.dll"), Qt::CaseInsensitive) == 0,
           QStringLiteral("W4b: .ts -> %1 (extensionPriority resolves the MPEG-TS/TypeScript clash)")
               .arg(QFileInfo(pm->pluginForExtension(QStringLiteral("ts"))).fileName()));

    // ---- W5: 大小写 / 带前导点都应当生效 ----
    report(!sWord.isEmpty()
               && pm->pluginForExtension(QStringLiteral(".DOCX")) == sWord,
           QStringLiteral("W5: case-insensitive and leading-dot tolerant"));

    // ---- W15: 插件类别（UI 角标/缩略图据此归类，主程序不再维护第二份后缀表） ----
    report(pm->categoryForExtension(QStringLiteral("docx")) == QStringLiteral("document")
               && pm->categoryForExtension(QStringLiteral("xlsx")) == QStringLiteral("sheet")
               && pm->categoryForExtension(QStringLiteral("pptx")) == QStringLiteral("slides")
               && pm->categoryForExtension(QStringLiteral("pdf")) == QStringLiteral("pdf")
               && pm->categoryForExtension(QStringLiteral("png")) == QStringLiteral("image")
               && pm->categoryForExtension(QStringLiteral("mp4")) == QStringLiteral("media")
               && pm->categoryForExtension(QStringLiteral("epub")) == QStringLiteral("book")
               && pm->categoryForExtension(QStringLiteral("txt")) == QStringLiteral("text")
               && pm->categoryForExtension(QStringLiteral("qzx")).isEmpty(),
           QStringLiteral("W15: plugin categories (document/sheet/slides/pdf/image/media/book/text)"));

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

    // ---- W7: 内容优先 —— 后缀写错时按文件头改路由；弱格式（zip/xml）不抢后缀 ----
    ProbeMainWindow mw;

    // PDF 被改名 .docx：必须交给 PDF 插件，而不是让 Word 插件去解析
    const ProbeMainWindow::RouteDecision pdfAsDocx =
        ProbeMainWindow::decideRoute(QStringLiteral("report.docx"),
                                     QByteArray("%PDF-1.7\n%\xE2\xE3\xCF\xD3\n", 15),
                                     sWord);
    report(QFileInfo(pdfAsDocx.pluginPath).fileName().compare(QStringLiteral("PluginPdf.dll"),
                                                              Qt::CaseInsensitive) == 0
               && pdfAsDocx.fromContent && pdfAsDocx.contentOverride,
           QStringLiteral("W7a: .docx whose content is PDF -> %1 (%2)")
               .arg(QFileInfo(pdfAsDocx.pluginPath).fileName(), pdfAsDocx.basis));

    // 修补过/超大 docx 在 256 KB 内没有 word/document.xml：内容只是"ZIP 容器"，
    // 不能因此把后缀声明（Word）挤掉
    const ProbeMainWindow::RouteDecision brokenDocx =
        ProbeMainWindow::decideRoute(QStringLiteral("broken.docx"),
                                     QByteArray("PK\x03\x04", 4) + QByteArray(64, '\0'), sWord);
    report(brokenDocx.pluginPath == sWord && !brokenDocx.fromContent
               && brokenDocx.basis.contains(QStringLiteral("docx")),
           QStringLiteral("W7b: generic ZIP keeps the declared .docx route"));

    // SVG 以 <?xml 开头，但内容是图片：不能被"XML 文档"抢走
    const QString sPicture = pm->pluginForExtension(QStringLiteral("svg"));
    const ProbeMainWindow::RouteDecision svgXml =
        ProbeMainWindow::decideRoute(QStringLiteral("icon.svg"),
            QByteArray("<?xml version=\"1.0\"?><svg xmlns=\"http://www.w3.org/2000/svg\"/>"),
            sPicture);
    report(svgXml.pluginPath == sPicture,
           QStringLiteral("W7c: SVG with XML prolog stays on the picture plugin"));

    // 没有后缀、内容也识别不出来：没有插件，调用方走「以文本查看」对话框
    const ProbeMainWindow::RouteDecision unknown =
        ProbeMainWindow::decideRoute(QStringLiteral("mystery"), QByteArray("hello"), QString());
    report(unknown.pluginPath.isEmpty(),
           QStringLiteral("W7d: unknown content without extension -> no plugin"));

    // 旧版 [PluginMap] 配置里的键必须彻底失效（功能已删除）
    {
        QSettings st(QSettings::IniFormat, QSettings::UserScope,
                     QStringLiteral("FileWrapper"), QStringLiteral("FileWrapper"));
        st.beginGroup(QStringLiteral("PluginMap"));
        st.setValue(QStringLiteral("qzx"), QStringLiteral("PluginText"));
        st.endGroup();
        st.sync();
    }
    const bool bIgnored = mw.choosePlugin(QStringLiteral("qzx")).isEmpty();
    {
        QSettings st(QSettings::IniFormat, QSettings::UserScope,
                     QStringLiteral("FileWrapper"), QStringLiteral("FileWrapper"));
        st.remove(QStringLiteral("PluginMap"));
        st.sync();
    }
    report(bIgnored, QStringLiteral("W7e: legacy PluginMap entry no longer routes .qzx"));
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
    const QString yDir = makeWritableTestDir(QStringLiteral("filechannel"));
    report(QDir(yDir).exists(), QStringLiteral("Y2a: temp dir for path source"));
    const QString sPayloadFile = yDir + QStringLiteral("/payload.bin");
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
    auto want = [&sOnly](const char *id, int nAssertions = 1) {
        const bool bWanted = sOnly.isEmpty() || sOnly == QLatin1String(id);
        if (!bWanted)
        {
            g_skipped += nAssertions;   // 按断言数计 SKIP，而不是按组计 1
            note(QStringLiteral("[SKIP] %1 skipped by FW_VIEWER_ONLY").arg(QLatin1String(id)));
        }
        return bWanted;
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
    if (want("V6", 3))
    {
        QPluginLoader loader(sPluginDir + QStringLiteral("/PluginWord.dll"));
        PluginInterface *templ = qobject_cast<PluginInterface *>(loader.instance());
        PluginInterface *p = templ ? templ->createInstance() : nullptr;
        bool bText = false;
        bool bTable = false;
        bool bRepeat = false;
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

        // ODT 的 number-rows/columns-repeated：2 行 ×（3 个 R + 1 个 Z）。
        PluginInterface *pRepeat = templ ? templ->createInstance() : nullptr;
        if (pRepeat != nullptr)
        {
            pRepeat->setSourceName(QStringLiteral("sample_odt_repeat.odt"));
            const QByteArray data = readFile(work + QStringLiteral("/sample_odt_repeat.odt"));
            pRepeat->sendFileData(const_cast<char *>(data.constData()), data.size());
            QApplication::processEvents();
            QTextDocument *doc = documentOf(pRepeat->getPluginWidget());
            if (doc != nullptr)
            {
                const QString plain = doc->toPlainText();
                const QString html = doc->toHtml();
                bRepeat = html.contains(QStringLiteral("<table"))
                           && plain.count(QLatin1Char('R')) == 6
                           && plain.count(QLatin1Char('Z')) == 2;
            }
            delete pRepeat;
        }

        report(bText, QStringLiteral("V6a: PluginWord renders .odt text (ODF)"));
        report(bTable, QStringLiteral("V6b: PluginWord renders .odt table"));
        report(bRepeat, QStringLiteral("V6c: ODT number-rows/columns-repeated is materialized"));
    }

    // V15: OPC 部件名的百分号解码（C3-14 回归）。三个最小包分别把主文档/
    // 幻灯片/工作表部件名写成 `%20`，ZIP 条目名是字面的空格；如果不做解码，
    // 三个插件会静默丢正文/整页/整表。
    if (want("V15", 3))
    {
        auto renderText = [&](const QString &sDll, const QString &sFile, QString *psText) {
            QPluginLoader loader(sPluginDir + QLatin1Char('/') + sDll);
            PluginInterface *pTemplate = qobject_cast<PluginInterface *>(loader.instance());
            if (pTemplate == nullptr) return false;
            PluginInterface *p = pTemplate->createInstance();
            if (p == nullptr) return false;
            const QByteArray data = readFile(sFile);
            p->sendFileData(const_cast<char *>(data.constData()), data.size());
            QApplication::processEvents();
            QTextDocument *pDoc = documentOf(p->getPluginWidget());
            if (pDoc != nullptr && psText != nullptr) *psText = pDoc->toPlainText();
            delete p;
            return pDoc != nullptr;
        };

        QString sWord, sSlides, sSheet;
        const bool bWord = renderText(QStringLiteral("PluginWord.dll"),
                                      work + QStringLiteral("/opc_percent.docx"), &sWord);
        const bool bSlides = renderText(QStringLiteral("PluginSlides.dll"),
                                        work + QStringLiteral("/opc_percent.pptx"), &sSlides);
        const bool bSheet = renderText(QStringLiteral("PluginSheet.dll"),
                                       work + QStringLiteral("/opc_percent.xlsx"), &sSheet);
        report(bWord && sWord.contains(QStringLiteral("OPC percent word")),
               QStringLiteral("V15a: docx main part with %20-encoded Target"));
        report(bSlides && sSlides.contains(QStringLiteral("OPC percent slide")),
               QStringLiteral("V15b: pptx slide part with %20-encoded Target"));
        report(bSheet && sSheet.contains(QStringLiteral("OPC percent sheet")),
               QStringLiteral("V15c: xlsx worksheet part with %20-encoded Target"));
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
    if (want("V8", 2))
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
                bBold = formatOfFragment(doc, QString::fromUtf8("粗体")).fontWeight() >= QFont::Bold;
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

    // V13: PluginMedia —— Stop/EOF 后按钮可重播；失败文件复位界面（V-01/V-03）
    if (want("V13"))
    {
        QPluginLoader loader(sPluginDir + QStringLiteral("/PluginMedia.dll"));
        PluginInterface *templ = qobject_cast<PluginInterface *>(loader.instance());
        PluginInterface *p = templ ? templ->createInstance() : nullptr;
        bool bOpen = false, bPlayingIcon = false, bStopIcon = false;
        bool bReplayIcon = false, bBadReset = false, bStopQuery = false;
        if (p != nullptr)
        {
            // 生成 0.5 秒静音 PCM WAV（合法容器，FFmpeg 可解析）
            auto putU32 = [](QByteArray &d, int nPos, quint32 v) {
                for (int i = 0; i < 4; ++i) d[nPos + i] = char((v >> (8 * i)) & 0xFF);
            };
            auto putU16 = [](QByteArray &d, int nPos, quint16 v) {
                for (int i = 0; i < 2; ++i) d[nPos + i] = char((v >> (8 * i)) & 0xFF);
            };
            const int nSamples = 44100 / 2;
            QByteArray wav(44 + nSamples * 2, '\0');
            std::memcpy(wav.data(), "RIFF", 4);
            putU32(wav, 4, quint32(36 + nSamples * 2));
            std::memcpy(wav.data() + 8, "WAVEfmt ", 8);
            putU32(wav, 16, 16);
            putU16(wav, 20, 1);
            putU16(wav, 22, 1);
            putU32(wav, 24, 44100);
            putU32(wav, 28, 44100 * 2);
            putU16(wav, 32, 2);
            putU16(wav, 34, 16);
            std::memcpy(wav.data() + 36, "data", 4);
            putU32(wav, 40, quint32(nSamples * 2));

            bOpen = p->sendFile(PluginFile::fromMemory(wav, QStringLiteral("audit.wav")));
            QWidget *pWin = p->getPluginWidget();
            auto *pPlay = pWin ? pWin->findChild<QPushButton *>(QStringLiteral("playPauseBtn")) : nullptr;
            auto *pStop = pWin ? pWin->findChild<QPushButton *>(QStringLiteral("stopBtn")) : nullptr;
            auto *pTime = pWin ? pWin->findChild<QLabel *>(QStringLiteral("timeLabel")) : nullptr;
            auto *pSlider = pWin ? pWin->findChild<QSlider *>(QStringLiteral("progressSlider")) : nullptr;
            auto *pVideo = pWin ? pWin->findChild<QLabel *>(QStringLiteral("videoLabel")) : nullptr;

            // P1-20：纯音频在音频设备不可用时必须“明确失败 + 复位界面”，
            // 而不是显示成正在播放。无声卡的 CI/远程环境走这条分支。
            if (!bOpen)
            {
                QApplication::processEvents();
                bStopQuery = p->isPlaybackStopped();   // v7：失败复位的实例必须已无后台线程
                const bool bRejectedCleanly = pTime != nullptr
                        && pTime->text() == QStringLiteral("0:00 / 0:00")
                        && pSlider != nullptr && !pSlider->isEnabled()
                        && pPlay != nullptr && pPlay->text() == QStringLiteral("▶")
                        && bStopQuery;
                note(QStringLiteral("[INFO] V13: no audio device; audio-only open correctly rejected"));
                report(bRejectedCleanly,
                       QStringLiteral("V13: audio-only open without device fails cleanly"));

                // v6：失败必须带上**真实原因**，并且原因要留在画面区。
                // 以前界面一律显示「插件无法显示该文件」——把"这台机器没声卡"
                // 说成"文件打不开"，用户照着这个提示永远修不好。
                const QString sWhy = p->lastError();
                const QString sVideoText = pVideo ? pVideo->text() : QString();
                report(!sWhy.isEmpty(),
                       QStringLiteral("V13b: failure reports the real reason ('%1')")
                           .arg(sWhy.left(40)));
                report(sVideoText.contains(QStringLiteral("音频")),
                       QStringLiteral("V13c: reason stays visible in the video area (not just the status bar)"));
            }
            else
            {
                for (int i = 0; i < 5; ++i) QApplication::processEvents();
                bPlayingIcon = pPlay != nullptr && pPlay->text() == QStringLiteral("⏸");

                if (pStop) pStop->click();
                QApplication::processEvents();
                bStopIcon = pPlay != nullptr && pPlay->text() == QStringLiteral("▶");
                bStopQuery = p->isPlaybackStopped();   // v7：停止确认后必须为 true

                if (pPlay) pPlay->click();   // 从起点重播
                for (int i = 0; i < 5; ++i) QApplication::processEvents();
                bReplayIcon = pPlay != nullptr && pPlay->text() == QStringLiteral("⏸");

                report(bPlayingIcon && bStopIcon && bReplayIcon && bStopQuery,
                       QStringLiteral("V13: media stop/replay button"));
            }

            // 坏数据：showFile 应返回 false、把进度/时间/按钮复位，并回报真实原因。
            // 这条与机器上有没有声卡无关，任何环境都必须成立。
            const QByteArray junk("this is not a media file", 24);
            const bool bRejected = !p->sendFile(PluginFile::fromMemory(junk, QStringLiteral("bad.mp4")));
            QApplication::processEvents();
            bBadReset = bRejected && pTime != nullptr
                        && pTime->text() == QStringLiteral("0:00 / 0:00")
                        && pSlider != nullptr && !pSlider->isEnabled()
                        && pPlay != nullptr && pPlay->text() == QStringLiteral("▶");
            report(bBadReset, QStringLiteral("V13d: bad media file resets the player UI"));

            const QString sBadWhy = p->lastError();
            const QString sBadPage = pVideo ? pVideo->text() : QString();
            report(!sBadWhy.isEmpty(),
                   QStringLiteral("V13e: bad media file reports why ('%1')").arg(sBadWhy.left(40)));
            report(sBadPage.contains(QStringLiteral("无法")),
                   QStringLiteral("V13f: bad media file shows the reason in the video area"));
        }
        delete p;
    }

    // V14: RTF \uN 后紧跟 \'hh 回退时不得把回退字节写进正文（P-07 回归）
    if (want("V14"))
    {
        QPluginLoader loader(sPluginDir + QStringLiteral("/PluginWord.dll"));
        PluginInterface *templ = qobject_cast<PluginInterface *>(loader.instance());
        PluginInterface *p = templ ? templ->createInstance() : nullptr;
        bool bOk = false;
        if (p != nullptr)
        {
            p->setSourceName(QStringLiteral("audit.rtf"));
            const QByteArray rtf("{\\rtf1\\ansi\\u2032\\'27}");
            p->sendFile(PluginFile::fromMemory(rtf, QStringLiteral("audit.rtf")));
            QWidget *w = p->getPluginWidget();
            auto *pBrowser = w ? w->findChild<QTextBrowser *>() : nullptr;
            auto *pEdit = w ? w->findChild<QTextEdit *>() : nullptr;
            const QString sText = pBrowser ? pBrowser->toPlainText()
                                           : (pEdit ? pEdit->toPlainText() : QString());
            // RTF 的 \uN 是十进制（\u2032 = U+07F0），不是十六进制；
            // 关键是 \'27 回退字节不能再被写进正文。
            bOk = sText.contains(QChar(2032)) && !sText.contains(QLatin1Char('\''));
        }
        delete p;
        report(bOk, QStringLiteral("V14: RTF unicode fallback byte is skipped"));
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

    // Z7: MPEG-TS 没有魔数：188 字节定长包、每包首字节 0x47。
    //     pluginExt 必须是 "m2ts" 而不是 "ts" —— 注册表里 "ts" 归 PluginText，
    //     返回 "ts" 会让内容识别结果又转回文本插件。
    QByteArray tsHead;
    for (int i = 0; i < 6; ++i)
    {
        tsHead += char(0x47);                       // sync byte
        tsHead += QByteArray("\x00\x00\x10", 3);    // PID=0, adaptation_field_control=01
        tsHead += QByteArray(184, '\x00');          // 184 字节载荷
    }
    const auto d10 = FormatResolver::detect(QStringLiteral("movie.ts"), tsHead);
    report(d10.formatId == QLatin1String("mpegts") && d10.pluginExt == QLatin1String("m2ts")
               && d10.fromContent,
           QStringLiteral("F7: content detects MPEG-TS -> %1").arg(d10.pluginExt));

    // Z8: 同一后缀下的 TypeScript 源码不能被当成视频（'G' 就是 0x47，纯文本里很常见）
    const auto d11 = FormatResolver::detect(QStringLiteral("code.ts"),
        QStringLiteral("import { a } from './b';\nexport const c = a + 1;\n").toUtf8());
    report(d11.formatId != QLatin1String("mpegts"),
           QStringLiteral("F8: TypeScript source not mistaken for MPEG-TS (%1)").arg(d11.formatId));

    // Z9: M2TS 变体（192 字节包：4 字节到达时间戳 + 188 字节 TS 包）也要认得
    QByteArray m2ts;
    for (int i = 0; i < 6; ++i)
    {
        m2ts += QByteArray("\x00\x00\x00\x00", 4);  // 192 字节包前的到达时间戳
        m2ts += char(0x47);
        m2ts += QByteArray("\x00\x00\x10", 3);
        m2ts += QByteArray(184, '\x00');
    }
    const auto d12 = FormatResolver::detect(QStringLiteral("cam.m2ts"), m2ts);
    report(d12.formatId == QLatin1String("mpegts"),
           QStringLiteral("F9: 192-byte M2TS detected (%1)").arg(d12.formatId));

    // Z10: SVG / FB2 也是 XML —— 不能被"XML 文档"判走（改名后的文件尤其常见），
    //      但 HTML 里内联的 <svg> 仍应归 HTML。
    const auto d13 = FormatResolver::detect(QStringLiteral("icon"),
        QByteArray("<?xml version=\"1.0\"?><svg xmlns=\"http://www.w3.org/2000/svg\"/>"));
    const auto d14 = FormatResolver::detect(QStringLiteral("book"),
        QByteArray("<?xml version=\"1.0\"?><FictionBook xmlns=\"http://www.gribuser.ru/xml/fictionbook/2.0\"/>"));
    const auto d15 = FormatResolver::detect(QStringLiteral("page.html"),
        QByteArray("<!doctype html><html><body><svg></svg></body></html>"));
    report(d13.formatId == QLatin1String("svg") && d13.pluginExt == QLatin1String("svg")
               && d14.formatId == QLatin1String("fb2") && d14.pluginExt == QLatin1String("fb2")
               && d15.formatId == QLatin1String("html"),
           QStringLiteral("F10: SVG/FB2 not mistaken for generic XML; inline SVG stays HTML"));

    // Z11: TIFF 魔数（II*\0 / MM\0*）—— .tif 以前连内容识别都没有
    const auto d16 = FormatResolver::detect(QStringLiteral("scan.dat"),
                                            QByteArray("II*\0rest", 8));
    report(d16.formatId == QLatin1String("tiff") && d16.pluginExt == QLatin1String("tiff")
               && d16.fromContent,
           QStringLiteral("F11: TIFF content detected -> %1").arg(d16.pluginExt));
}

// 缩略图/角标这类 UI 归类：必须来自插件元数据，而不是主程序里第二份手工后缀表
// （旧表漏了 tga/icns/wbmp，用户能打开却不显示缩略图）。
static void verifyUiTypeHelpers()
{
    report(ProbeTreeWindow::isThumbnailImageName(QStringLiteral("a.png"))
               && ProbeTreeWindow::isThumbnailImageName(QStringLiteral("a.tga"))
               && ProbeTreeWindow::isThumbnailImageName(QStringLiteral("a.icns"))
               && ProbeTreeWindow::isThumbnailImageName(QStringLiteral("a.wbmp"))
               && ProbeTreeWindow::isThumbnailImageName(QStringLiteral("a.jfif"))
               && !ProbeTreeWindow::isThumbnailImageName(QStringLiteral("a.txt"))
               && !ProbeTreeWindow::isThumbnailImageName(QStringLiteral("noext")),
           QStringLiteral("U1: thumbnail suffixes follow plugin category/Qt decoder list"));
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
    using MainWindow::pluginStatusText;
    using MainWindow::supportedFormatsText;
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
    const bool bSpaceProbe = FileManager::hasFreeSpaceFor(sTemp, 1, &nAvail);
    report(bSpaceProbe && nAvail > 0,
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
    // M-10：禁用项不再重试，但仍保留一条"已禁用（未重试）"记录，
    // 避免诊断里既不是失败也不是禁用；关键是不能被重新加载。
    const bool bStillRecorded = !pm->failureFor(sBadDll).isEmpty();
    const bool bDisabledText = pm->failureFor(sBadDll).contains(QStringLiteral("已禁用"));
    report(bRecorded && nRetried == 0 && bStillRecorded && bDisabledText,
           QStringLiteral("X6: disabled plugin not resurrected (recorded=%1 retried=%2 disabledText=%3)")
               .arg(bRecorded).arg(nRetried).arg(bDisabledText));

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

    // H5: 分割对话框支持预填"待分割文件路径"（已加载归档时主窗口会传入）
    DlgSplitFile splitDlg;
    splitDlg.setMergedFilePath(QStringLiteral("E:/tmp/prefilled.dat"));
    report(splitDlg.getMergedFilePath() == QStringLiteral("E:/tmp/prefilled.dat"),
           QStringLiteral("H5: split dialog accepts prefilled archive path"));

    // H6: 主窗口注册了常用快捷键（加载/合并/保存）
    MainWindow mw;
    bool bHasOpen = false, bHasMerge = false, bHasSave = false;
    for (QAction *pAct : mw.findChildren<QAction *>())
    {
        if (pAct->shortcut() == QKeySequence(QKeySequence::Open)) bHasOpen = true;
        if (pAct->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_M)) bHasMerge = true;
        if (pAct->shortcut() == QKeySequence(QKeySequence::Save)) bHasSave = true;
    }
    report(bHasOpen && bHasMerge && bHasSave,
           QStringLiteral("H6: window shortcuts registered (Ctrl+O/M/S)"));

    // H7: 大小列按数值排序（"99.16 KB" 不应排在 "989 B" 前）
    FileListModel sizeModel;
    sizeModel.setColumnCount(3);
    auto addSizeRow = [&sizeModel](const QString &sName, qint64 nBytes, const QString &sText) {
        auto *pName = new QStandardItem(sName);
        auto *pSize = new QStandardItem(sText);
        pSize->setData(nBytes, Qt::UserRole);
        auto *pType = new QStandardItem(QStringLiteral("T"));
        sizeModel.appendRow(QList<QStandardItem *>() << pName << pSize << pType);
    };
    addSizeRow(QStringLiteral("a"), 989, QStringLiteral("989 B"));
    addSizeRow(QStringLiteral("b"), 101556, QStringLiteral("99.16 KB"));
    addSizeRow(QStringLiteral("c"), 998, QStringLiteral("998 B"));
    sizeModel.sort(1, Qt::AscendingOrder);
    bool bSizeSortOk = sizeModel.item(0, 0)->text() == QStringLiteral("a")
                       && sizeModel.item(1, 0)->text() == QStringLiteral("c")
                       && sizeModel.item(2, 0)->text() == QStringLiteral("b");
    sizeModel.sort(1, Qt::DescendingOrder);
    bSizeSortOk = bSizeSortOk
                  && sizeModel.item(0, 0)->text() == QStringLiteral("b")
                  && sizeModel.item(2, 0)->text() == QStringLiteral("a");
    report(bSizeSortOk,
           QStringLiteral("H7: size column sorts numerically (989 < 998 < 99.16KB)"));

    // H8: 平铺/树形都启用列排序（树形为"同级排序、目录优先"，不会拍平）
    auto *pView = mw.findChild<QTreeView *>();
    QMetaObject::invokeMethod(&mw, "onActionViewFlatTriggered", Qt::DirectConnection);
    const bool bFlatEnabled = pView != nullptr && pView->isSortingEnabled();
    QMetaObject::invokeMethod(&mw, "onActionViewTreeTriggered", Qt::DirectConnection);
    const bool bTreeEnabled = pView != nullptr && pView->isSortingEnabled();
    report(pView != nullptr && bFlatEnabled && bTreeEnabled,
           QStringLiteral("H8: both flat and tree modes enable column sorting"));

    // H9: 树形排序——同级排序、目录优先、大小按子树合计字节、递归到子目录
    FileListModel treeModel;
    treeModel.setColumnCount(3);
    auto makeRow = [](const QString &sName, qint64 nBytes, bool bDir) -> QList<QStandardItem *> {
        auto *pName = new QStandardItem(sName);
        if (bDir)
            pName->setData(true, FileListModel::IsDirRole);
        auto *pSize = new QStandardItem(QString::number(nBytes));
        pSize->setData(nBytes, FileListModel::BytesRole);
        auto *pType = new QStandardItem(bDir ? QStringLiteral("文件夹") : QStringLiteral("TXT"));
        return QList<QStandardItem *>() << pName << pSize << pType;
    };
    QList<QStandardItem *> rootRow = makeRow(QStringLiteral("root"), 3159, true);
    treeModel.appendRow(rootRow);
    QStandardItem *pRootItem = rootRow.at(0);
    pRootItem->appendRow(makeRow(QStringLiteral("a.txt"), 50, false));
    pRootItem->appendRow(makeRow(QStringLiteral("b.txt"), 999, false));
    QList<QStandardItem *> zdirRow = makeRow(QStringLiteral("zdir"), 100, true);
    pRootItem->appendRow(zdirRow);
    zdirRow.at(0)->appendRow(makeRow(QStringLiteral("z.txt"), 100, false));
    QList<QStandardItem *> adirRow = makeRow(QStringLiteral("adir"), 2010, true);
    pRootItem->appendRow(adirRow);
    adirRow.at(0)->appendRow(makeRow(QStringLiteral("d1.txt"), 2000, false));
    adirRow.at(0)->appendRow(makeRow(QStringLiteral("d0.txt"), 10, false));

    treeModel.sort(1, Qt::AscendingOrder);
    QStringList treeTopAsc;
    for (int r = 0; r < pRootItem->rowCount(); ++r)
        treeTopAsc << pRootItem->child(r, 0)->text();
    const QStandardItem *pAdirItem = pRootItem->child(1, 0);
    bool bTreeSortOk = (treeTopAsc == QStringList{ QStringLiteral("zdir"),
                                                   QStringLiteral("adir"),
                                                   QStringLiteral("a.txt"),
                                                   QStringLiteral("b.txt") });
    bTreeSortOk = bTreeSortOk && pAdirItem != nullptr
                  && pAdirItem->text() == QStringLiteral("adir")
                  && pAdirItem->child(0, 0)->text() == QStringLiteral("d0.txt")
                  && pAdirItem->child(1, 0)->text() == QStringLiteral("d1.txt");

    treeModel.sort(1, Qt::DescendingOrder);
    QStringList treeTopDesc;
    for (int r = 0; r < pRootItem->rowCount(); ++r)
        treeTopDesc << pRootItem->child(r, 0)->text();
    bTreeSortOk = bTreeSortOk && (treeTopDesc == QStringList{ QStringLiteral("adir"),
                                                              QStringLiteral("zdir"),
                                                              QStringLiteral("b.txt"),
                                                              QStringLiteral("a.txt") });
    report(bTreeSortOk,
           QStringLiteral("H9: tree sort is per-level, directories first, size numeric/recursive"));

    // H10: 真实 MainWindow 树形填充——目录"合计大小/文件夹"、同级目录优先排序
    const QString sTreeDir = [] {
        return QDir::tempPath() + QStringLiteral("/fw_tree_sort_test_%1")
                                    .arg(QCoreApplication::applicationPid());
    }();
    QDir(sTreeDir).removeRecursively();
    QDir().mkpath(sTreeDir + QStringLiteral("/sub"));
    auto writeBytes = [](const QString &sPath, int n) {
        QFile f(sPath);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        return f.write(QByteArray(n, 'x')) == n;
    };
    const bool bFiles = writeBytes(sTreeDir + QStringLiteral("/sub/a.bin"), 50)
                        && writeBytes(sTreeDir + QStringLiteral("/sub/b.bin"), 999)
                        && writeBytes(sTreeDir + QStringLiteral("/top.txt"), 1234);
    const QString sTreeArc = sTreeDir + QStringLiteral("/tree.dat");
    FileManager fmTree;
    if (bFiles)
    {
        fmTree.QMergeFiles(QVector<QString>{ sTreeDir + QStringLiteral("/sub/a.bin"),
                                            sTreeDir + QStringLiteral("/sub/b.bin"),
                                            sTreeDir + QStringLiteral("/top.txt") },
                           sTreeArc);
    }
    const bool bMerged = bFiles && QFileInfo::exists(sTreeArc);
    bool bTreeUi = false;
    if (bMerged)
    {
        ProbeTreeWindow mwTree;
        // 测试环境可能沿用用户上次保存的平铺模式：先显式切到树形再加载
        QMetaObject::invokeMethod(&mwTree, "onActionViewTreeTriggered", Qt::DirectConnection);
        mwTree.loadArchiveFile(sTreeArc);
        auto *pTreeView = mwTree.findChild<QTreeView *>();
        auto *pModel = pTreeView ? qobject_cast<QStandardItemModel *>(pTreeView->model())
                                 : nullptr;
        QStandardItem *pRootItem = pModel ? pModel->item(0, 0) : nullptr;
        QStandardItem *pSubItem = nullptr;
        if (pRootItem)
        {
            for (int r = 0; r < pRootItem->rowCount(); ++r)
            {
                QStandardItem *pChild = pRootItem->child(r, 0);
                if (pChild && pChild->text().endsWith(QStringLiteral("sub")))
                    pSubItem = pChild;
            }
        }
        bTreeUi = pRootItem != nullptr
                  && pRootItem->data(FileListModel::IsDirRole).toBool()
                  && pModel->item(0, 1) != nullptr
                  && pModel->item(0, 1)->data(FileListModel::BytesRole).toLongLong() == 2283
                  && pModel->item(0, 2) != nullptr
                  && pModel->item(0, 2)->text() == QStringLiteral("文件夹")
                  && pSubItem != nullptr
                  && pSubItem->data(FileListModel::IsDirRole).toBool();

        if (bTreeUi)
        {
            // 按大小降序：目录永远在前（sub 1049 在 top.txt 1234 之前），目录内 999 > 50
            pModel->sort(1, Qt::DescendingOrder);
            const QString sFirst = pRootItem->child(0, 0) ? pRootItem->child(0, 0)->text() : QString();
            const QString sSecond = pRootItem->child(1, 0) ? pRootItem->child(1, 0)->text() : QString();
            int nSubRow = -1;
            for (int r = 0; r < pRootItem->rowCount(); ++r)
                if (pRootItem->child(r, 0) == pSubItem)
                    nSubRow = r;
            const bool bSubSize = nSubRow >= 0
                                  && pRootItem->child(nSubRow, 1) != nullptr
                                  && pRootItem->child(nSubRow, 1)->data(FileListModel::BytesRole).toLongLong() == 1049;
            const bool bSubType = nSubRow >= 0
                                  && pRootItem->child(nSubRow, 2) != nullptr
                                  && pRootItem->child(nSubRow, 2)->text() == QStringLiteral("文件夹");
            const QString sSubFirst = pSubItem->child(0, 0) ? pSubItem->child(0, 0)->text() : QString();
            const QString sSubSecond = pSubItem->child(1, 0) ? pSubItem->child(1, 0)->text() : QString();
            bTreeUi = sFirst.endsWith(QStringLiteral("sub"))
                      && sSecond.endsWith(QStringLiteral("top.txt"))
                      && bSubSize && bSubType
                      && sSubFirst.endsWith(QStringLiteral("b.bin"))
                      && sSubSecond.endsWith(QStringLiteral("a.bin"));
        }
    }
    report(bTreeUi,
           QStringLiteral("H10: real tree view shows dir totals and sorts dirs-first per level"));

    // H11: 树形点头列头排序不改变目录的展开/收起状态
    bool bExpansionKept = false;
    if (bMerged)
    {
        ProbeTreeWindow mwExp;
        QMetaObject::invokeMethod(&mwExp, "onActionViewTreeTriggered", Qt::DirectConnection);
        mwExp.loadArchiveFile(sTreeArc);
        mwExp.show();
        QApplication::processEvents();
        auto *pExpView = mwExp.findChild<QTreeView *>();
        auto *pExpModel = pExpView ? qobject_cast<QStandardItemModel *>(pExpView->model())
                                   : nullptr;
        QStandardItem *pExpRoot = pExpModel ? pExpModel->item(0, 0) : nullptr;
        QStandardItem *pExpSub = nullptr;
        if (pExpRoot)
        {
            for (int r = 0; r < pExpRoot->rowCount(); ++r)
            {
                QStandardItem *pChild = pExpRoot->child(r, 0);
                if (pChild && pChild->text().endsWith(QStringLiteral("sub")))
                    pExpSub = pChild;
            }
        }
        if (pExpSub)
        {
            const QModelIndex subIdx = pExpModel->indexFromItem(pExpSub);
            pExpView->expand(subIdx);
            QApplication::processEvents();
            const bool bExpandedBefore = pExpView->isExpanded(subIdx);

            // 点击"大小"列头：按下捕获展开状态，松开后排序，再恢复
            QHeaderView *pHeader = pExpView->header();
            pHeader->resizeSection(1, 120);
            QApplication::processEvents();
            const QPoint pos(pHeader->sectionViewportPosition(1) + 20,
                             pHeader->height() / 2);
            const QPoint globalPos = pHeader->viewport()->mapToGlobal(pos);
            QMouseEvent press(QEvent::MouseButtonPress, pos, globalPos,
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(pHeader->viewport(), &press);
            QMouseEvent release(QEvent::MouseButtonRelease, pos, globalPos,
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(pHeader->viewport(), &release);
            QApplication::processEvents();

            // 排序会移动行；按名称重新找到 sub 再检查展开状态
            QStandardItem *pExpSubAfter = nullptr;
            for (int r = 0; r < pExpRoot->rowCount(); ++r)
            {
                QStandardItem *pChild = pExpRoot->child(r, 0);
                if (pChild && pChild->text().endsWith(QStringLiteral("sub")))
                    pExpSubAfter = pChild;
            }
            const bool bExpandedAfter = pExpSubAfter != nullptr
                && pExpView->isExpanded(pExpModel->indexFromItem(pExpSubAfter));
            bExpansionKept = bExpandedBefore && bExpandedAfter
                             && pHeader->sortIndicatorSection() == 1;
        }
    }
    report(bExpansionKept,
           QStringLiteral("H11: header-click sort keeps expanded directories expanded"));

    // H12: FILEINFO 用显式 InfoRole，目录不再污染 data() 默认角色（M-01 回归）
    bool bRoleSeparation = false;
    if (bMerged)
    {
        ProbeTreeWindow mwRole;
        QMetaObject::invokeMethod(&mwRole, "onActionViewTreeTriggered", Qt::DirectConnection);
        mwRole.loadArchiveFile(sTreeArc);
        auto *pRoleView = mwRole.findChild<QTreeView *>();
        auto *pRoleModel = pRoleView ? qobject_cast<QStandardItemModel *>(pRoleView->model())
                                     : nullptr;
        QStandardItem *pRoleRoot = pRoleModel ? pRoleModel->item(0, 0) : nullptr;
        QStandardItem *pRoleFile = nullptr;
        if (pRoleRoot)
        {
            for (int r = 0; r < pRoleRoot->rowCount(); ++r)
            {
                QStandardItem *pChild = pRoleRoot->child(r, 0);
                if (pChild && pChild->text().endsWith(QStringLiteral("top.txt")))
                    pRoleFile = pChild;
            }
        }
        bRoleSeparation = pRoleRoot != nullptr
                          && !pRoleRoot->data(FileListModel::InfoRole).isValid()
                          && pRoleRoot->data(FileListModel::IsDirRole).toBool()
                          && pRoleFile != nullptr
                          && pRoleFile->data(FileListModel::InfoRole).isValid()
                          && pRoleFile->data(FileListModel::InfoRole).value<FILEINFO>().sFileName
                                 == QStringLiteral("top.txt")
                          && !pRoleFile->data(FileListModel::IsDirRole).toBool();
    }
    report(bRoleSeparation,
           QStringLiteral("H12: FILEINFO uses InfoRole; dir role no longer aliases default data() role"));

    // H13: 单链目录压缩后不得残留空壳行（M-02 回归）
    const QString sChainDir = QDir::tempPath() + QStringLiteral("/fw_chain_test_%1").arg(QCoreApplication::applicationPid());
    QDir(sChainDir).removeRecursively();
    QDir().mkpath(sChainDir + QStringLiteral("/sub/deep"));
    const bool bChainFiles = writeBytes(sChainDir + QStringLiteral("/sub/deep/only.txt"), 100)
                             && writeBytes(sChainDir + QStringLiteral("/top.txt"), 200);
    const QString sChainArc = sChainDir + QStringLiteral("/chain.dat");
    FileManager fmChain;
    if (bChainFiles)
        fmChain.QMergeFiles(QVector<QString>{ sChainDir + QStringLiteral("/sub/deep/only.txt"),
                                             sChainDir + QStringLiteral("/top.txt") },
                            sChainArc);
    bool bNoGhost = false;
    if (bChainFiles && QFileInfo::exists(sChainArc))
    {
        ProbeTreeWindow mwChain;
        QMetaObject::invokeMethod(&mwChain, "onActionViewTreeTriggered", Qt::DirectConnection);
        mwChain.loadArchiveFile(sChainArc);
        auto *pChainView = mwChain.findChild<QTreeView *>();
        auto *pChainModel = pChainView ? qobject_cast<QStandardItemModel *>(pChainView->model())
                                       : nullptr;
        QStandardItem *pChainRoot = pChainModel ? pChainModel->item(0, 0) : nullptr;
        QStandardItem *pChainDir = nullptr;
        if (pChainRoot)
        {
            for (int r = 0; r < pChainRoot->rowCount(); ++r)
            {
                QStandardItem *pChild = pChainRoot->child(r, 0);
                if (pChild && pChild->text().endsWith(QStringLiteral("sub/deep")))
                    pChainDir = pChild;
            }
        }
        // 期望：root 下只有 [sub/deep 目录, top.txt]；sub/deep 下只有 [only.txt]，没有空 "deep" 壳
        bNoGhost = pChainRoot != nullptr && pChainRoot->rowCount() == 2
                   && pChainDir != nullptr && pChainDir->rowCount() == 1
                   && pChainDir->child(0, 0) != nullptr
                   && pChainDir->child(0, 0)->text().endsWith(QStringLiteral("only.txt"));
    }
    report(bNoGhost,
           QStringLiteral("H13: single-chain dir compaction leaves no empty ghost row"));

    // H14: 树形刷新（筛选变化）后已展开目录保持展开（M-05 回归）
    bool bRefreshKeepsExpansion = false;
    if (bMerged)
    {
        ProbeTreeWindow mwRefresh;
        QMetaObject::invokeMethod(&mwRefresh, "onActionViewTreeTriggered", Qt::DirectConnection);
        mwRefresh.loadArchiveFile(sTreeArc);
        mwRefresh.show();
        QApplication::processEvents();
        auto *pRefreshView = mwRefresh.findChild<QTreeView *>();
        auto *pRefreshModel = pRefreshView ? qobject_cast<QStandardItemModel *>(pRefreshView->model())
                                           : nullptr;
        auto *pFilterEdit = mwRefresh.findChild<QLineEdit *>(QStringLiteral("leFilter"));
        QStandardItem *pRefreshRoot = pRefreshModel ? pRefreshModel->item(0, 0) : nullptr;
        QStandardItem *pRefreshSub = nullptr;
        if (pRefreshRoot)
        {
            for (int r = 0; r < pRefreshRoot->rowCount(); ++r)
            {
                QStandardItem *pChild = pRefreshRoot->child(r, 0);
                if (pChild && pChild->text().endsWith(QStringLiteral("sub")))
                    pRefreshSub = pChild;
            }
        }
        if (pRefreshSub && pFilterEdit)
        {
            pRefreshView->expand(pRefreshModel->indexFromItem(pRefreshSub));
            QApplication::processEvents();
            pFilterEdit->setText(QFileInfo(QDir::tempPath() + QStringLiteral("/fw_tree_sort_test_%1")
                                           .arg(QCoreApplication::applicationPid())).fileName());
            QApplication::processEvents();
            // 筛选会重建模型：必须从新模型重新取根，旧指针已失效
            QStandardItem *pRefreshRootAfter = pRefreshModel->item(0, 0);
            QStandardItem *pRefreshSubAfter = nullptr;
            if (pRefreshRootAfter)
            {
                for (int r = 0; r < pRefreshRootAfter->rowCount(); ++r)
                {
                    QStandardItem *pChild = pRefreshRootAfter->child(r, 0);
                    if (pChild && pChild->text().endsWith(QStringLiteral("sub")))
                        pRefreshSubAfter = pChild;
                }
            }
            bRefreshKeepsExpansion = pRefreshSubAfter != nullptr
                && pRefreshView->isExpanded(pRefreshModel->indexFromItem(pRefreshSubAfter));
            pFilterEdit->clear();
            QApplication::processEvents();
        }
    }
    report(bRefreshKeepsExpansion,
           QStringLiteral("H14: tree refresh keeps expanded dirs expanded"));

    // H15: 导出对话框——切回"原路径导出"清空旧目录；选择目录模式正常返回
    bool bOutputDialog = false;
    {
        DlgOutputFile dlgOld;
        auto *pEdit = dlgOld.findChild<QLineEdit *>(QStringLiteral("leOutputFileDir"));
        auto *pOld = dlgOld.findChild<QRadioButton *>(QStringLiteral("rbOldPath"));
        auto *pOk = dlgOld.findChild<QPushButton *>(QStringLiteral("btnOk"));
        if (pEdit && pOld && pOk)
        {
            pEdit->setText(QStringLiteral("D:/tmp/export"));
            pOld->click();     // 切回原路径：应清空文本框
            pOk->click();      // 确定：输出目录必须为空
            bOutputDialog = pEdit->text().isEmpty() && dlgOld.getOutputDir().isEmpty();
        }
        DlgOutputFile dlgSel;
        auto *pEdit2 = dlgSel.findChild<QLineEdit *>(QStringLiteral("leOutputFileDir"));
        auto *pSel = dlgSel.findChild<QRadioButton *>(QStringLiteral("rbSelectDir"));
        auto *pOk2 = dlgSel.findChild<QPushButton *>(QStringLiteral("btnOk"));
        if (pEdit2 && pSel && pOk2)
        {
            pSel->click();
            pEdit2->setText(QStringLiteral("D:/tmp/export"));
            pOk2->click();
            bOutputDialog = bOutputDialog
                            && dlgSel.getOutputDir() == QStringLiteral("D:/tmp/export");
        }
        else
        {
            bOutputDialog = false;
        }
    }
    report(bOutputDialog,
           QStringLiteral("H15: output dialog clears stale dir on old-path and returns selected dir"));

    // H16: 输出路径实时解析 + 长路径随窗口宽度换行
    {
        auto *pEdit = dlg.findChild<QLineEdit *>(QStringLiteral("leMergedFilePath"));
        auto *pHint = dlg.findChild<QLabel *>(QStringLiteral("labelResolvedPath"));
        bool bResponsive = false;
        if (pEdit && pHint)
        {
            // 用一条没有空格的长路径验证：窗口变窄时 label 必须增高，不能裁掉末行。
            pEdit->setText(QStringLiteral(
                "E:/study_lujiajing/WorkSpace_FileWrapper/FileWrapper/build/"
                "Desktop_Qt_6_9_3_MinGW_64_bit_Debug/FileWrapper/debug/merged-output"));
            dlg.resize(860, 470);
            dlg.show();
            for (int i = 0; i < 5; ++i) QApplication::processEvents();
            const int nWideHeight = pHint->height();
            dlg.resize(360, 470);
            for (int i = 0; i < 5; ++i) QApplication::processEvents();
            const int nNarrowHeight = pHint->height();
            bResponsive = nWideHeight > 0 && nNarrowHeight > nWideHeight
                          && pHint->height() == pHint->heightForWidth(pHint->width());
        }
        const QString sExpect = QDir::cleanPath(QFileInfo(QStringLiteral("a")).absoluteFilePath());
        if (pEdit) pEdit->setText(QStringLiteral("a"));
        const QString sHint = pHint ? pHint->text() : QString();
        report(pEdit != nullptr && pHint != nullptr && bResponsive && sHint.contains(sExpect),
               QStringLiteral("H16: relative path resolved and long path wraps on resize (%1)")
                   .arg(QString(sHint).replace(QLatin1Char('\n'), QStringLiteral(" | "))));
    }

    // H17: 没写扩展名的输出补 .fwda；已有扩展名（.fwda / 老的 .dat / "v1.2" 版本号）原样保留
    {
        const QString sNoExt  = DlgMergeFile::ensureArchiveSuffix(QStringLiteral("E:/tmp/merged"));
        const QString sDotEnd = DlgMergeFile::ensureArchiveSuffix(QStringLiteral("E:/tmp/merged."));
        const QString sFwda   = DlgMergeFile::ensureArchiveSuffix(QStringLiteral("E:/tmp/merged.fwda"));
        const QString sDat    = DlgMergeFile::ensureArchiveSuffix(QStringLiteral("E:/tmp/merged.dat"));
        const QString sVer    = DlgMergeFile::ensureArchiveSuffix(QStringLiteral("E:/tmp/v1.2"));
        report(sNoExt == QStringLiteral("E:/tmp/merged.fwda")
                   && sDotEnd == QStringLiteral("E:/tmp/merged.fwda")
                   && sFwda == QStringLiteral("E:/tmp/merged.fwda")
                   && sDat == QStringLiteral("E:/tmp/merged.dat")
                   && sVer == QStringLiteral("E:/tmp/v1.2")
                   && FwArc::hasArchiveSuffix(QStringLiteral("x.fwda"))
                   && FwArc::hasArchiveSuffix(QStringLiteral("x.DAT"))
                   && !FwArc::hasArchiveSuffix(QStringLiteral("x.png")),
               QStringLiteral("H17: missing output extension becomes .fwda (%1)").arg(sNoExt));
    }

    // H18: 加载/另存为/拆分共用的过滤器必须同时列出 *.fwda 与老的 *.dat，
    //      否则「另存成 .fwda」之后老归档在对话框里就看不见了
    {
        const QString sFilter = FwArc::fileDialogFilter();
        report(sFilter.contains(QStringLiteral("*.fwda"))
                   && sFilter.contains(QStringLiteral("*.dat")),
               QStringLiteral("H18: archive filter lists both *.fwda and *.dat (%1)").arg(sFilter));
    }

    // H19: 文件列表右键菜单——文件行 = 打开 / 打开新标签页 / 以文本查看 / 以十六进制查看 / 属性；
    //      目录行 = 属性；同时校验「属性」数据源（名称/类型/位置/大小/CRC/归档格式）
    bool bContextMenuOk = false;
    bool bFileDataOk = false;
    bool bFolderDataOk = false;
    bool bTagColumnOk = false;
    if (bMerged)
    {
        ProbeTreeWindow mwProp;
        QMetaObject::invokeMethod(&mwProp, "onActionViewTreeTriggered", Qt::DirectConnection);
        mwProp.loadArchiveFile(sTreeArc);
        auto *pPropView = mwProp.findChild<QTreeView *>();
        auto *pPropModel = pPropView
                               ? qobject_cast<QStandardItemModel *>(pPropView->model())
                               : nullptr;
        bTagColumnOk = pPropModel != nullptr && pPropModel->columnCount() == 4
                       && pPropModel->headerData(3, Qt::Horizontal).toString()
                              == QStringLiteral("标记");
        QStandardItem *pPropRoot = pPropModel ? pPropModel->item(0, 0) : nullptr;
        QStandardItem *pPropFile = nullptr;
        QStandardItem *pPropDir = nullptr;
        if (pPropRoot)
        {
            for (int r = 0; r < pPropRoot->rowCount(); ++r)
            {
                QStandardItem *pChild = pPropRoot->child(r, 0);
                if (pChild == nullptr) continue;
                if (pChild->text().endsWith(QStringLiteral("top.txt"))) pPropFile = pChild;
                if (pChild->text().endsWith(QStringLiteral("sub")))     pPropDir = pChild;
            }
        }

        auto actionTexts = [](QMenu *pMenu) {
            QStringList texts;
            if (pMenu != nullptr)
            {
                for (QAction *pAction : pMenu->actions())
                    if (!pAction->isSeparator())
                        texts << pAction->text();
            }
            return texts;
        };

        QStringList fileActs;
        QStringList dirActs;
        if (pPropFile)
        {
            QMenu *pMenu = mwProp.buildFileListContextMenu(
                pPropModel->indexFromItem(pPropFile));
            fileActs = actionTexts(pMenu);
            delete pMenu;
        }
        if (pPropDir)
        {
            QMenu *pMenu = mwProp.buildFileListContextMenu(
                pPropModel->indexFromItem(pPropDir));
            dirActs = actionTexts(pMenu);
            delete pMenu;
        }
        bContextMenuOk = (fileActs == QStringList{ QStringLiteral("打开"),
                                                   QStringLiteral("打开新标签页"),
                                                   QStringLiteral("标记…"),
                                                   QStringLiteral("以文本查看"),
                                                   QStringLiteral("以十六进制查看"),
                                                   QStringLiteral("属性") })
                         && (dirActs == QStringList{ QStringLiteral("属性") });

        if (pPropFile)
        {
            const FILEINFO fi = pPropFile->data(FileListModel::InfoRole).value<FILEINFO>();
            const DlgFileProperty::Data fd = mwProp.filePropertyData(fi);
            bFileDataOk = fd.sName == QStringLiteral("top.txt")
                          && fd.sType == QStringLiteral("TXT 文件")
                          && fd.nSize == 1234
                          && fd.sLocation == FwArc::normalizePath(sTreeDir)
                          && fd.sEntryPath == FwArc::normalizePath(sTreeDir + QStringLiteral("/top.txt"))
                          && fd.sArchivePath == FwArc::normalizePath(sTreeArc)
                          && fd.nArchiveVersion == FwArc::CURRENT_VERSION
                          && fd.listTags.isEmpty()
                          && fd.bHasFsMetadata
                          && fd.nModifiedTime > 0
                          && fd.bHasCrc
                          && !fd.bPending
                          && !fd.bDir;
        }
        if (pPropDir && pPropRoot)
        {
            const DlgFileProperty::Data subData = mwProp.folderPropertyData(
                pPropModel->indexFromItem(pPropDir));
            const DlgFileProperty::Data rootData = mwProp.folderPropertyData(
                pPropModel->indexFromItem(pPropRoot));
            // writeBytes: sub/a.bin=50, sub/b.bin=999, top.txt=1234
            bFolderDataOk = subData.bDir
                            && subData.sName == QStringLiteral("sub")
                            && subData.nSize == 1049
                            && subData.nFileCount == 2
                            && subData.nDirCount == 0
                            && subData.sLocation == FwArc::normalizePath(sTreeDir)
                            && subData.sEntryPath == FwArc::normalizePath(sTreeDir + QStringLiteral("/sub"))
                            && !subData.bFiltered
                            && rootData.bDir
                            && rootData.nSize == 2283
                            && rootData.nFileCount == 3
                            && rootData.nDirCount == 1
                            && rootData.sLocation.isEmpty();
        }
    }
    report(bContextMenuOk,
           QStringLiteral("H19a: file-list context menu offers 属性 for files and dirs"));
    report(bFileDataOk,
           QStringLiteral("H19b: file property data carries name/type/size/location/CRC"));
    report(bFolderDataOk,
           QStringLiteral("H19c: dir property data aggregates visible subtree size/counts"));
    report(bTagColumnOk,
           QStringLiteral("H19d: file list exposes a 标记 column (4 columns)"));

    // H20: 属性文本——大小格式、CRC、待保存条目、v1 旧归档、文件夹汇总与筛选说明
    bool bPropTextOk = false;
    bool bPropStateOk = false;
    bool bPropFolderOk = false;
    {
        DlgFileProperty::Data d;
        d.sName = QStringLiteral("report.docx");
        d.sType = DlgFileProperty::typeTextForFile(d.sName);
        d.sLocation = QStringLiteral("E:/testfiles_copy");
        d.nSize = 22907;
        d.sArchivePath = QStringLiteral("E:/testfiles_copy/1.dat");
        d.nArchiveVersion = FwArc::VERSION_V3;
        d.sEntryPath = QStringLiteral("E:/testfiles_copy/report.docx");
        d.bHasCrc = true;
        d.nCrc = 0xDEADBEEFu;
        d.bHasFsMetadata = true;
        d.nCreatedTime = QDateTime(QDate(2025, 1, 25), QTime(15, 53)).toMSecsSinceEpoch();
        d.nModifiedTime = QDateTime(QDate(2025, 1, 26), QTime(16, 54)).toMSecsSinceEpoch();
        d.nAccessedTime = QDateTime(QDate(2025, 1, 27), QTime(17, 55)).toMSecsSinceEpoch();
        d.nAttributes = FwMeta::ATTR_ARCHIVE | FwMeta::ATTR_READONLY;

        const QString sText = DlgFileProperty::formatInfoText(d);
        bPropTextOk = sText.contains(QStringLiteral("名称：report.docx"))
                      && sText.contains(QStringLiteral("类型：DOCX 文件"))
                      && sText.contains(QStringLiteral("位置：E:/testfiles_copy"))
                      && sText.contains(QStringLiteral("大小：22.37 KB（22,907 字节）"))
                      && sText.contains(QStringLiteral("压缩方式：不压缩（原样存储）"))
                      && sText.contains(QStringLiteral("创建时间：2025/1/25"))
                      && sText.contains(QStringLiteral("修改时间：2025/1/26"))
                      && sText.contains(QStringLiteral("访问时间：2025/1/27"))
                      && sText.contains(QStringLiteral("属性：RA"))
                      && sText.contains(QStringLiteral("所属归档：E:/testfiles_copy/1.dat"))
                      && sText.contains(QStringLiteral("归档格式：FWDA v3"))
                      && sText.contains(QStringLiteral("归档内路径：E:/testfiles_copy/report.docx"))
                      && sText.contains(QStringLiteral("内容校验：CRC32 0xDEADBEEF"))
                      && sText.contains(QStringLiteral("状态：已保存在归档中"))
                      && DlgFileProperty::formatSize(0) == QStringLiteral("0 字节")
                      && DlgFileProperty::formatSize(1024 * 1024)
                             == QStringLiteral("1.00 MB（1,048,576 字节）")
                      && DlgFileProperty::typeTextForFile(QStringLiteral("README"))
                             == QStringLiteral("文件")
                      && sText.contains(QStringLiteral("标记：—"));

        // v4：归档格式写 v4，标记按顿号连接显示
        DlgFileProperty::Data tagged = d;
        tagged.nArchiveVersion = FwArc::CURRENT_VERSION;
        tagged.listTags = QStringList{ QStringLiteral("合同"), QStringLiteral("重要") };
        const QString sTagged = DlgFileProperty::formatInfoText(tagged);
        bPropTextOk = bPropTextOk
                      && sTagged.contains(QStringLiteral("归档格式：FWDA v4"))
                      && sTagged.contains(QStringLiteral("标记：合同、重要"));

        DlgFileProperty::Data pending = d;
        pending.bHasCrc = false;
        pending.nCrc = 0;
        pending.bPending = true;
        const QString sPending = DlgFileProperty::formatInfoText(pending);

        DlgFileProperty::Data v1 = d;
        v1.bHasCrc = false;
        v1.nCrc = 0;
        v1.bHasFsMetadata = false;
        v1.nArchiveVersion = FwArc::VERSION_V1;
        const QString sV1 = DlgFileProperty::formatInfoText(v1);
        bPropStateOk = sPending.contains(QStringLiteral("内容校验：不可用（新增条目尚未保存）"))
                       && sPending.contains(QStringLiteral("状态：新增条目，尚未保存"))
                       && sV1.contains(QStringLiteral("归档格式：FWDA v1（旧格式）"))
                       && sV1.contains(QStringLiteral("内容校验：不可用（v1 归档未记录内容校验）"))
                       && sV1.contains(QStringLiteral("状态：已保存在归档中"))
                       && sV1.contains(QStringLiteral("文件时间：未记录（该归档版本未保存文件元数据）"));

        DlgFileProperty::Data folderData;
        folderData.bDir = true;
        folderData.sName = QStringLiteral("代码");
        folderData.sType = QStringLiteral("文件夹");
        folderData.sLocation = QStringLiteral("E:/study");
        folderData.nSize = 1060000;
        folderData.nFileCount = 20;
        folderData.nDirCount = 4;
        folderData.bFiltered = true;
        folderData.sArchivePath = QStringLiteral("E:/study/1.dat");
        folderData.nArchiveVersion = FwArc::VERSION_V3;
        folderData.sEntryPath = QStringLiteral("E:/study/代码");
        const QString sFolder = DlgFileProperty::formatInfoText(folderData);
        DlgFileProperty::Data unfiltered;
        unfiltered.bDir = true;
        unfiltered.nFileCount = 1;
        bPropFolderOk = sFolder.contains(QStringLiteral("类型：文件夹"))
                        && sFolder.contains(QStringLiteral("大小（合计）：1.01 MB（1,060,000 字节）"))
                        && sFolder.contains(QStringLiteral("包含：20 个文件、4 个子文件夹"))
                        && sFolder.contains(QStringLiteral("统计范围：当前搜索筛选结果"))
                        && sFolder.contains(QStringLiteral("归档内路径：E:/study/代码"))
                        && !sFolder.contains(QStringLiteral("内容校验"))
                        && !sFolder.contains(QStringLiteral("状态："))
                        && DlgFileProperty::formatInfoText(unfiltered)
                               .contains(QStringLiteral("统计范围：当前列表中的归档条目"));
    }
    report(bPropTextOk,
           QStringLiteral("H20a: property text formats size/CRC/archive fields"));
    report(bPropStateOk,
           QStringLiteral("H20b: pending entry and v1 archive show explicit CRC/state text"));
    report(bPropFolderOk,
           QStringLiteral("H20c: folder property text shows aggregate and filter scope"));

    // H21: 文件列表第三种视图——缩略图（图标网格 + 图片内容预览 + 非图片角标回退）
    {
        const QString sThumbDir = makeWritableTestDir(QStringLiteral("thumb_view"));
        const QString sPng = sThumbDir + QStringLiteral("/preview.png");
        const QString sPng2 = sThumbDir + QStringLiteral("/a_preview.png");
        const QString sTxt = sThumbDir + QStringLiteral("/readme.txt");
        const QString sArc = sThumbDir + QStringLiteral("/thumb.dat");

        QImage img(120, 80, QImage::Format_RGB32);
        img.fill(QColor(QStringLiteral("#2E8EF1")));
        const bool bPng = img.save(sPng, "PNG") && img.save(sPng2, "PNG");
        bool bTxt = false;
        {
            QFile f(sTxt);
            if (f.open(QIODevice::WriteOnly))
                bTxt = f.write("thumbnail fallback") > 0;
        }

        bool bMerged = false;
        if (bPng && bTxt)
        {
            FileManager fmThumb;
            QObject::connect(&fmThumb, &FileManager::operationFinished,
                             [&bMerged](bool o, const QString &) { bMerged = o; });
            // 故意让合并顺序与名称排序不同：applyCurrentSortToModel() 会 takeRow/appendRow，
            // 用来覆盖“排序后持久索引失效导致缩略图不再生成”的回归。
            fmThumb.QMergeFiles(QVector<QString>{ sPng, sTxt, sPng2 }, sArc);
            bMerged = bMerged && QFileInfo::exists(sArc);
        }

        bool bGridOk = false;
        bool bImageOk = false;
        bool bFallbackAndAsyncOk = false;
        bool bBackToTreeOk = false;
        int nRows = -1;
        if (bMerged)
        {
            ProbeTreeWindow mwThumb;
            QMetaObject::invokeMethod(&mwThumb, "onActionViewThumbnailTriggered",
                                      Qt::DirectConnection);
            mwThumb.loadArchiveFile(sArc);

            auto *pList = mwThumb.findChild<QListView *>(QStringLiteral("thumbnailView"));
            auto *pStack = mwThumb.findChild<QStackedWidget *>(QStringLiteral("fileViewStack"));
            auto *pTree = mwThumb.findChild<QTreeView *>();
            auto *pModel = pList ? qobject_cast<QStandardItemModel *>(pList->model()) : nullptr;
            nRows = pModel ? pModel->rowCount() : -1;

            QStandardItem *pPngItem = nullptr;
            QStandardItem *pPngItem2 = nullptr;
            QStandardItem *pTxtItem = nullptr;
            if (pModel)
            {
                for (int r = 0; r < pModel->rowCount(); ++r)
                {
                    QStandardItem *pItem = pModel->item(r, 0);
                    if (pItem == nullptr)
                        continue;
                    if (pItem->text() == QStringLiteral("preview.png")) pPngItem = pItem;
                    if (pItem->text() == QStringLiteral("a_preview.png")) pPngItem2 = pItem;
                    if (pItem->text() == QStringLiteral("readme.txt")) pTxtItem = pItem;
                }
            }

            bGridOk = pList != nullptr && pStack != nullptr && pTree != nullptr
                      && pStack->currentWidget() == pList
                      && pList->viewMode() == QListView::IconMode
                      && pList->model() == pModel
                      && pModel != nullptr && pModel->rowCount() == 3
                      && pList->iconSize().width() >= 64
                      && pPngItem != nullptr && pPngItem2 != nullptr && pTxtItem != nullptr;

            if (pPngItem)
            {
                const FILEINFO fi = pPngItem->data(FileListModel::InfoRole).value<FILEINFO>();
                const QPixmap pm = mwThumb.renderEntryThumbnail(fi);
                bImageOk = !pm.isNull() && pm.width() >= 96
                           && pm.width() <= 192 && pm.height() <= 192;
            }
            if (pTxtItem)
                bFallbackAndAsyncOk = !pTxtItem->icon().isNull();

            // 缩略图由空闲回调逐张生成：记录占位角标的 cacheKey，等图片格子换成新图标。
            // 只判断“有图标”不行——占位角标本来就有；必须确认图标对象被替换过。
            const qint64 nPngKey = pPngItem ? pPngItem->icon().cacheKey() : 0;
            const qint64 nPng2Key = pPngItem2 ? pPngItem2->icon().cacheKey() : 0;
            if (pPngItem && pPngItem2)
            {
                QElapsedTimer wait;
                wait.start();
                while (wait.elapsed() < 5000)
                {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                    if (pPngItem->icon().cacheKey() != nPngKey
                        && pPngItem2->icon().cacheKey() != nPng2Key)
                    {
                        bFallbackAndAsyncOk = true;
                        break;
                    }
                }
            }

            if (pList && pStack && pTree)
            {
                QMetaObject::invokeMethod(&mwThumb, "onActionViewTreeTriggered",
                                          Qt::DirectConnection);
                auto *pTreeModel = qobject_cast<QStandardItemModel *>(pTree->model());
                bBackToTreeOk = pStack->currentWidget() == pTree
                                && pTreeModel != nullptr
                                && pTreeModel->item(0, 0) != nullptr
                                && pTreeModel->item(0, 0)->data(FileListModel::IsDirRole).toBool()
                                && pTree->isSortingEnabled();
            }
        }
        report(bGridOk,
               QStringLiteral("H21a: thumbnail view is a third icon-grid mode (%1 rows)").arg(nRows));
        report(bImageOk, QStringLiteral("H21b: image entry decodes to a real thumbnail pixmap"));
        report(bFallbackAndAsyncOk,
               QStringLiteral("H21c: non-image keeps type badge, images get real thumbnails"));
        report(bBackToTreeOk, QStringLiteral("H21d: switching back to tree restores tree model"));
        QDir(sThumbDir).removeRecursively();
    }

    QDir(sChainDir).removeRecursively();
}

// ---------------------------------------------------------------------------
// K: 保存时的路径规划（添加文件后保存不得拍平目录 / 不得产生重复路径）
// ---------------------------------------------------------------------------
static void verifySavePathPreservation(const QString &work, const QString &scratchDir)
{
    auto listPaths = [](const FwArc::Meta &m) {
        QStringList l;
        for (const FwArc::Entry &e : m.entries) l << e.relPath;
        return l;
    };
    auto hasDup = [](const QStringList &paths) {
        QSet<QString> seen;
        for (const QString &p : paths) {
            const QString k = p.toLower();
            if (seen.contains(k)) return true;
            seen.insert(k);
        }
        return false;
    };
    auto writeNote = [](const QString &path, const QByteArray &body) {
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(body);
    };

    // K1: 跨盘合并（E: 素材 + C: 临时文件）后，再添加一个 C: 文件并保存：
    //     必须保持"盘符当首段"的扁平路径，绝不能退化成裸文件名。
    const QString sCross = scratchDir + QStringLiteral("/save_cross.dat");
    const QString sNote  = scratchDir + QStringLiteral("/save_note.txt");
    const QString sAdded = scratchDir + QStringLiteral("/save_added.txt");
    writeNote(sNote, "note");
    writeNote(sAdded, "added");

    FileManager fm1;
    fm1.QMergeFiles(QVector<QString>{ work + QStringLiteral("/basic.docx"), sNote }, sCross);
    FileManager fm2;
    fm2.QLoadMergedFile(sCross);
    fm2.addFiles(QVector<QString>{ sAdded });
    const bool bSaved1 = fm2.save();
    FwArc::Meta crossMeta;
    QString err1;
    const bool bMeta1 = FileManager::inspectArchive(sCross, crossMeta, err1);
    const QStringList crossPaths = listPaths(crossMeta);
    bool bHasSlash = !crossPaths.isEmpty();
    for (const QString &p : crossPaths)
        if (!p.contains(QLatin1Char('/'))) bHasSlash = false;
    const QString expectedRoot = FwArc::commonRoot(
        QStringList{ work + QStringLiteral("/basic.docx"), sNote });
    const bool bRootShape = (crossMeta.root == expectedRoot);
    const bool bFlatShape = !expectedRoot.isEmpty()
        || (!crossPaths.contains(QStringLiteral("basic.docx")) && bHasSlash);
    report(bSaved1 && bMeta1 && bRootShape && bFlatShape && crossPaths.size() == 3
               && !hasDup(crossPaths),
           QStringLiteral("K1: cross-drive save keeps drive-prefixed paths (%1)")
               .arg(crossPaths.join(QStringLiteral(", "))));

    // K4: archivePath 返回当前加载的归档路径（供分割对话框预填）
    report(fm2.archivePath().endsWith(QStringLiteral("save_cross.dat")),
           QStringLiteral("K4: archivePath returns loaded archive (%1)").arg(fm2.archivePath()));

    // K2: 同一盘内添加"归档公共根之外"的文件后保存：
    //     公共根应上移且各条目保留子目录层级（fixtures/...），不能拍平成文件名。
    const QString sSame = scratchDir + QStringLiteral("/save_same.dat");
    // 用 scratch 的上一级目录里自建文件作为“公共根之外”的来源：该目录由运行器
    // 创建，在受限沙箱里也可写（fixtures 的上级目录是源码树，不应依赖可写）。
    const QString sUp = QFileInfo(scratchDir + QStringLiteral("/../k2_sibling.txt"))
                            .absoluteFilePath();
    {
        QFile fUp(sUp);
        fUp.open(QIODevice::WriteOnly | QIODevice::Truncate);
        fUp.write("k2 sibling");
    }
    if (true)
    {
        FileManager fm3;
        fm3.QMergeFiles(QVector<QString>{ work + QStringLiteral("/basic.docx"),
                                          work + QStringLiteral("/pic.png") }, sSame);
        FileManager fm4;
        fm4.QLoadMergedFile(sSame);
        fm4.addFiles(QVector<QString>{ sUp });
        const bool bSaved2 = fm4.save();
        FwArc::Meta sameMeta;
        QString err2;
        const bool bMeta2 = FileManager::inspectArchive(sSame, sameMeta, err2);
        const QStringList samePaths = listPaths(sameMeta);
        bool bKeptDirs = samePaths.size() == 3;
        for (const QString &p : samePaths)
        {
            if (p == QStringLiteral("basic.docx") || p == QStringLiteral("pic.png"))
                bKeptDirs = false;   // 出现了裸文件名 -> 被拍平
        }
        // 跨盘（scratch 与 fixtures 不同卷）时 root 为空、盘符当首段也是合法形态，
        // 这里只断言「不拍平成裸文件名」；root 非空只在同卷时要求。
        const bool bRootShapeOk = !samePaths.isEmpty()
                                  && (!sameMeta.root.isEmpty()
                                      || samePaths.first().contains(QLatin1Char('/')));
        report(bSaved2 && bMeta2 && bRootShapeOk && bKeptDirs && !hasDup(samePaths),
               QStringLiteral("K2: save keeps subdirectories (%1)")
                   .arg(samePaths.join(QStringLiteral(", "))));
        QFile::remove(sUp);   // 不把临时来源文件留进 fixtures 上级目录
    }

    // K3: 归档内已有重复路径时，保存必须被拒绝并给出可读原因（不能静默拍平/覆盖）
    const QString sDup = work + QStringLiteral("/dup_paths.dat");
    if (QFileInfo::exists(sDup))
    {
        FileManager fm5;
        // A-03 后改为在加载阶段直接拒绝重复路径，避免 QMap 单值覆盖后两行同内容、
        // 删一条让幸存条目变成“归档内未找到”。
        const bool bLoad3 = fm5.QLoadMergedFile(sDup);
        const QString sErr3 = fm5.lastError();
        report(!bLoad3 && sErr3.contains(QStringLiteral("重复路径")),
               QStringLiteral("K3: duplicate stored paths reject load (%1)").arg(sErr3));
    }
    else
    {
        // 缺素材时不能把整条断言静默吞掉（否则退出码仍可能为 0）
        report(false, QStringLiteral("K3: missing fixture dup_paths.dat"));
    }

    // K5: 取消标志复位：上一次"取消"不应让后续导出/大文件打开立刻失败
    if (FileManager::inspectArchive(sCross, crossMeta, err1))
    {
        FileManager fm6;
        fm6.QLoadMergedFile(sCross);
        fm6.cancelOperation();
        const bool bCancelled = fm6.isCancelRequested();
        fm6.resetCancel();
        const bool bReset = !fm6.isCancelRequested();
        // 导出用加载后的 FILEINFO 路径（与 UI 双击导出完全同一条链路），
        // 不能用 Meta.relPath：同盘合并时 root 非空，relPath 是「相对 root」的
        // 形式，和 outputFile() 的查找键 sFilePath 不是一回事（跨盘时才恰好相等）。
        const QVector<FILEINFO> infos6 = fm6.getFileInfos();
        const QString sExportPath = infos6.isEmpty() ? QString() : infos6.first().sFilePath;
        fm6.outputFile(sExportPath, scratchDir);
        report(bCancelled && bReset && fm6.lastError().isEmpty(),
               QStringLiteral("K5: cancel flag reset lets next export succeed (%1)")
                   .arg(fm6.lastError().isEmpty()
                            ? sExportPath : fm6.lastError()));
    }

    // K6: 删除后重加同路径文件不得复用旧 CRC（A-02）
    {
        const QString sSrc = scratchDir + QStringLiteral("/crc_readd.bin");
        const QString sArc = scratchDir + QStringLiteral("/crc_readd.dat");
        auto write = [](const QString &p, char c) {
            QFile f(p);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
            return f.write(QByteArray(64, c)) == 64;
        };
        write(sSrc, 'A');
        FileManager fm;
        fm.QMergeFiles(QVector<QString>{sSrc}, sArc);
        fm.QLoadMergedFile(sArc);
        QString sPath = fm.getFileInfos().isEmpty() ? QString() : fm.getFileInfos().first().sFilePath;
        const bool bDel = fm.deleteFile(sPath);
        write(sPath, 'B');
        fm.addFiles(QVector<QString>{sPath});
        QString sErr;
        const QByteArray data = fm.entryContent(sPath, &sErr);
        report(bDel && !data.isEmpty() && !sErr.contains(QStringLiteral("CRC")),
               QStringLiteral("K6: re-added file after delete does not reuse stale CRC (%1)").arg(sErr));
    }

    // K7: 合并输出不能是当前已加载归档（A-13）
    {
        const QString sSrc = scratchDir + QStringLiteral("/merge_loaded.bin");
        const QString sOther = scratchDir + QStringLiteral("/merge_other.bin");
        const QString sArc = scratchDir + QStringLiteral("/merge_loaded.dat");
        auto write = [](const QString &p, char c) {
            QFile f(p);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
            return f.write(QByteArray(32, c)) == 32;
        };
        write(sSrc, 'a');
        write(sOther, 'b');
        FileManager fm;
        fm.QMergeFiles(QVector<QString>{sSrc}, sArc);
        fm.QLoadMergedFile(sArc);
        bool bDone = false, bOk = true;
        QString sMsg;
        QObject::connect(&fm, &FileManager::operationFinished,
                         [&](bool ok, const QString &m) { bDone = true; bOk = ok; sMsg = m; });
        fm.QMergeFiles(QVector<QString>{sOther}, sArc);
        report(bDone && !bOk && sMsg.contains(QStringLiteral("当前已加载")),
               QStringLiteral("K7: merge refuses currently loaded archive as output"));
    }

    // K8: 合并输出不能是已存在目录（A-23）
    {
        const QString sSrc = scratchDir + QStringLiteral("/merge_dir.bin");
        const QString sOut = scratchDir + QStringLiteral("/merge_out_dir");
        QFile f(sSrc);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write(QByteArray(32, 'd'));
        f.close();
        QDir().mkpath(sOut);
        FileManager fm;
        bool bDone = false, bOk = true;
        QString sMsg;
        QObject::connect(&fm, &FileManager::operationFinished,
                         [&](bool ok, const QString &m) { bDone = true; bOk = ok; sMsg = m; });
        fm.QMergeFiles(QVector<QString>{sSrc}, sOut);
        report(bDone && !bOk && sMsg.contains(QStringLiteral("目录")),
               QStringLiteral("K8: merge refuses directory as output"));
    }

    // K9: 合并成功的提示必须带输出的绝对路径——相对路径按当前目录解析，
    // 提示里不写这一行，用户只能靠猜才知道合并结果落在哪（2026-11-19 真实踩过）。
    {
        const QString sSrc = scratchDir + QStringLiteral("/merge_msg_src.bin");
        const QString sOut = scratchDir + QStringLiteral("/merge_msg_out");
        {
            QFile f(sSrc);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                f.write(QByteArray(16, 'm'));
        }
        FileManager fm;
        bool bDone = false, bOk = false;
        QString sMsg;
        QObject::connect(&fm, &FileManager::operationFinished,
                         [&](bool ok, const QString &m) { bDone = true; bOk = ok; sMsg = m; });
        fm.QMergeFiles(QVector<QString>{sSrc}, sOut);
        const QString sExpect = QFileInfo(sOut).absoluteFilePath();
        report(bDone && bOk && sMsg.contains(QStringLiteral("输出文件：")) && sMsg.contains(sExpect),
               QStringLiteral("K9: success message shows absolute output path (%1)")
                   .arg(QString(sMsg).replace(QLatin1Char('\n'), QStringLiteral(" | "))));
        QFile::remove(sOut);
        QFile::remove(sSrc);
    }

    // K10: 后缀只是命名约定——.fwda 归档必须能原样加载回来（识别一律看文件头魔数 FWDA，
    //      不依赖后缀；老的 .dat 同理，所以换后缀不需要迁移任何旧文件）
    {
        const QString sSrc = scratchDir + QStringLiteral("/fwda_suffix_src.bin");
        const QString sArc = scratchDir + QStringLiteral("/fwda_suffix.fwda");
        {
            QFile f(sSrc);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                f.write(QByteArray(64, 's'));
        }
        FileManager fm;
        bool bMergeOk = false;
        QObject::connect(&fm, &FileManager::operationFinished,
                         [&bMergeOk](bool ok, const QString &) { bMergeOk = ok; });
        fm.QMergeFiles(QVector<QString>{sSrc}, sArc);
        const bool bLoaded = fm.QLoadMergedFile(sArc);
        const int nEntries = fm.getFileInfos().size();
        report(bMergeOk && bLoaded && nEntries == 1,
               QStringLiteral("K10: .fwda archive loads by magic bytes (%1 entries, merge=%2, load=%3)")
                   .arg(nEntries).arg(bMergeOk).arg(bLoaded));
        QFile::remove(sArc);
        QFile::remove(sSrc);
    }
}

// ---------------------------------------------------------------------------
// P: FWDA v3 文件系统元数据
//   合并时把创建/修改/访问时间与 DOS 属性写进归档；导出/分割时写回磁盘；
//   v1/v2 归档仍可读取（没有元数据）。
// ---------------------------------------------------------------------------
static void verifyArchiveMetadata(const QString &work, const QString &scratchDir)
{
    Q_UNUSED(scratchDir);
    const QString sDir = makeWritableTestDir(QStringLiteral("archive_meta"));
    const bool bDirOk = QDir(sDir).exists();
    report(bDirOk, QStringLiteral("P0: metadata test dir"));
    if (!bDirOk)
        return;

    auto writeFile = [](const QString &sPath, const QByteArray &data) {
        QFile f(sPath);
        return f.open(QIODevice::WriteOnly | QIODevice::Truncate)
               && f.write(data) == data.size();
    };
    // 设置修改/访问时间（QFileDevice::setFileTime 是实例方法，需要先打开文件）
    auto setTimes = [](const QString &sPath, const QDateTime &dtMod, const QDateTime &dtAcc) {
        QFile f(sPath);
        if (!f.open(QIODevice::ReadWrite))
            return false;
        bool bOk = f.setFileTime(dtMod, QFileDevice::FileModificationTime);
        bOk = f.setFileTime(dtAcc, QFileDevice::FileAccessTime) && bOk;
        f.close();
        return bOk;
    };
    auto near = [](qint64 a, qint64 b) { return qAbs(a - b) <= 2000; };
    auto clearReadOnlyForCleanup = [](const QString &sRoot) {
        QDirIterator it(sRoot, QDir::Files | QDir::Hidden | QDir::System,
                        QDirIterator::Subdirectories);
        while (it.hasNext())
            QFile::setPermissions(it.next(), QFile::ReadOwner | QFile::WriteOwner
                                               | QFile::ReadUser | QFile::WriteUser);
    };

    const QString sAlpha = sDir + QStringLiteral("/alpha.txt");
    const QString sBeta  = sDir + QStringLiteral("/beta.txt");
    const QDateTime dtModified(QDate(2025, 2, 3), QTime(4, 5, 6));
    const QDateTime dtAccessed(QDate(2025, 2, 4), QTime(5, 6, 7));
    const bool bSources = writeFile(sAlpha, QByteArray(300, 'a'))
                          && writeFile(sBeta, QByteArray(700, 'b'))
                          && setTimes(sAlpha, dtModified, dtAccessed)
                          && setTimes(sBeta, dtModified, dtAccessed);
    // 把 beta 设为只读：Windows 上就是 FILE_ATTRIBUTE_READONLY
    const bool bReadOnly = bSources
                           && QFile::setPermissions(sBeta, QFile::ReadOwner | QFile::ReadUser);
    report(bSources && bReadOnly, QStringLiteral("P1: metadata source files ready"));
    if (!bSources || !bReadOnly)
    {
        clearReadOnlyForCleanup(sDir);
        QDir(sDir).removeRecursively();
        return;
    }

    FileManager fm;
    const QString sArc = sDir + QStringLiteral("/meta.fwda");
    bool bMergeDone = false, bMergeOk = false;
    QObject::connect(&fm, &FileManager::operationFinished,
                     [&](bool ok, const QString &) { bMergeDone = true; bMergeOk = ok; });
    fm.QMergeFiles(QVector<QString>{ sAlpha, sBeta }, sArc);

    FwArc::Meta meta;
    QString sErr;
    const bool bInspect = FileManager::inspectArchive(sArc, meta, sErr);
    report(bMergeDone && bMergeOk && bInspect
               && meta.version == FwArc::VERSION_V4 && meta.entries.size() == 2,
           QStringLiteral("P1b: merge writes FWDA v4 (%1 entries, err=%2)")
               .arg(bInspect ? meta.entries.size() : -1).arg(sErr));

    auto entryIndexBySuffix = [&meta](const QString &sSuffix) {
        for (int i = 0; i < meta.entries.size(); ++i)
            if (meta.entries.at(i).displayPath.endsWith(sSuffix, Qt::CaseInsensitive))
                return i;
        return -1;
    };
    const int iAlpha = entryIndexBySuffix(QStringLiteral("alpha.txt"));
    const int iBeta  = entryIndexBySuffix(QStringLiteral("beta.txt"));
    const qint64 nModMs = dtModified.toMSecsSinceEpoch();
    const bool bHeaderMeta = iAlpha >= 0 && iBeta >= 0
        && meta.entries.at(iAlpha).hasFsMetadata
        && near(qint64(meta.entries.at(iAlpha).modifiedMs), nModMs)
        // 访问时间会被「读文件」等操作刷新（Windows 也可能在句柄关闭时回写），
        // 这里只要求它被记录成一个有效时间，不比对具体值。
        && meta.entries.at(iAlpha).accessedMs > 0
        && (meta.entries.at(iAlpha).attributes & FwMeta::ATTR_ARCHIVE)
        && (meta.entries.at(iBeta).attributes & FwMeta::ATTR_READONLY);
    report(bHeaderMeta,
           QStringLiteral("P2: v3 header stores times/attributes "
                          "[iA=%1 iB=%2 meta=%3 mod=%4/%5 acc=%6 attrA=0x%7 attrB=0x%8]")
               .arg(iAlpha).arg(iBeta)
               .arg(iAlpha >= 0 ? meta.entries.at(iAlpha).hasFsMetadata : false)
               .arg(iAlpha >= 0 ? qint64(meta.entries.at(iAlpha).modifiedMs) : -1)
               .arg(nModMs)
               .arg(iAlpha >= 0 ? qint64(meta.entries.at(iAlpha).accessedMs) : -1)
               .arg(iAlpha >= 0 ? meta.entries.at(iAlpha).attributes : 0, 0, 16)
               .arg(iBeta >= 0 ? meta.entries.at(iBeta).attributes : 0, 0, 16));

    FileManager fmLoad;
    const bool bLoaded = fmLoad.QLoadMergedFile(sArc);
    const QVector<FILEINFO> infos = fmLoad.getFileInfos();
    auto findInfo = [&infos](const QString &sSuffix) {
        for (const FILEINFO &fi : infos)
            if (fi.sFilePath.endsWith(sSuffix, Qt::CaseInsensitive))
                return fi;
        return FILEINFO();
    };
    const FILEINFO fiAlpha = findInfo(QStringLiteral("alpha.txt"));
    const FILEINFO fiBeta  = findInfo(QStringLiteral("beta.txt"));
    report(bLoaded && fiAlpha.bHasFsMetadata && near(fiAlpha.nModifiedTime, nModMs)
               && fiAlpha.nAccessedTime > 0
               && (fiBeta.nAttributes & FwMeta::ATTR_READONLY),
           QStringLiteral("P3: loaded FILEINFO exposes v3 metadata "
                          "[loaded=%1 meta=%2 mod=%3 acc=%4 attrB=0x%5]")
               .arg(bLoaded).arg(fiAlpha.bHasFsMetadata)
               .arg(fiAlpha.nModifiedTime).arg(fiAlpha.nAccessedTime)
               .arg(fiBeta.nAttributes, 0, 16));

    // —— 导出：时间/属性写回 ——
    const QString sOutDir = sDir + QStringLiteral("/out");
    QDir().mkpath(sOutDir);
    fmLoad.outputFile(fiAlpha.sFilePath, sOutDir);
    const QString sAlphaOut = fmLoad.lastOutputPath();
    const bool bExportTime = !sAlphaOut.isEmpty() && QFileInfo::exists(sAlphaOut)
        && near(QFileInfo(sAlphaOut).lastModified().toMSecsSinceEpoch(), nModMs)
        && (iAlpha < 0 || meta.entries.at(iAlpha).createdMs == 0
            || near(QFileInfo(sAlphaOut).birthTime().toMSecsSinceEpoch(),
                    qint64(meta.entries.at(iAlpha).createdMs)));
    report(bExportTime, QStringLiteral("P4: export restores modified+created time (%1)")
                            .arg(QFileInfo(sAlphaOut).fileName()));

    fmLoad.outputFile(fiBeta.sFilePath, sOutDir);
    const QString sBetaOut = fmLoad.lastOutputPath();
    const bool bExportRo = !sBetaOut.isEmpty() && QFileInfo::exists(sBetaOut)
                           && !QFileInfo(sBetaOut).isWritable();
    report(bExportRo, QStringLiteral("P5: export restores read-only attribute"));

    // —— 分割：同样恢复时间/属性 ——
    const QString sSplitDir = sDir + QStringLiteral("/split");
    bool bSplitDone = false, bSplitOk = false;
    QString sSplitMsg;
    QObject::connect(&fmLoad, &FileManager::operationFinished,
                     [&](bool ok, const QString &msg) { bSplitDone = true; bSplitOk = ok; sSplitMsg = msg; });
    fmLoad.QSplitFiles(sArc, false, sSplitDir);

    QString sSplitAlpha, sSplitBeta;
    QDirIterator splitIt(sSplitDir, QDir::Files, QDirIterator::Subdirectories);
    while (splitIt.hasNext())
    {
        const QString sPath = splitIt.next();
        if (sPath.endsWith(QStringLiteral("alpha.txt"), Qt::CaseInsensitive)) sSplitAlpha = sPath;
        if (sPath.endsWith(QStringLiteral("beta.txt"), Qt::CaseInsensitive))  sSplitBeta  = sPath;
    }
    const bool bSplitMeta = bSplitDone && bSplitOk
        && !sSplitAlpha.isEmpty() && !sSplitBeta.isEmpty()
        && near(QFileInfo(sSplitAlpha).lastModified().toMSecsSinceEpoch(), nModMs)
        && !QFileInfo(sSplitBeta).isWritable();
    report(bSplitMeta, QStringLiteral("P6: split restores times/attributes (%1)")
                           .arg(QString(sSplitMsg).replace(QLatin1Char('\n'), QStringLiteral(" | "))));

    // —— 新增文件：添加时捕获、保存时落盘 ——
    const QString sGamma = sDir + QStringLiteral("/gamma.txt");
    const QDateTime dtGammaMod = dtModified.addSecs(3600);
    const bool bGammaSrc = writeFile(sGamma, QByteArray(50, 'g'))
                           && setTimes(sGamma, dtGammaMod, dtAccessed);
    if (bGammaSrc)
        fmLoad.addFiles(QVector<QString>{ sGamma });
    const bool bAdded = bGammaSrc;
    FILEINFO fiGamma;
    for (const FILEINFO &fi : fmLoad.getFileInfos())
        if (fi.sFilePath.endsWith(QStringLiteral("gamma.txt"), Qt::CaseInsensitive))
            fiGamma = fi;
    const bool bAddedMeta = bAdded && fiGamma.bHasFsMetadata
                            && near(fiGamma.nModifiedTime, dtGammaMod.toMSecsSinceEpoch());
    const bool bSaved = bAddedMeta && fmLoad.save();

    FwArc::Meta metaSaved;
    QString sSavedErr;
    const bool bSavedInspect = bSaved && FileManager::inspectArchive(sArc, metaSaved, sSavedErr);
    bool bSavedMeta = false;
    if (bSavedInspect)
    {
        for (const FwArc::Entry &e : metaSaved.entries)
        {
            if (!e.displayPath.endsWith(QStringLiteral("gamma.txt"), Qt::CaseInsensitive))
                continue;
            bSavedMeta = e.hasFsMetadata
                         && near(qint64(e.modifiedMs), dtGammaMod.toMSecsSinceEpoch())
                         && (e.attributes & FwMeta::ATTR_ARCHIVE);
        }
    }
    report(bSavedMeta, QStringLiteral("P7: pending add captures metadata and save persists it"));

    // —— v2 旧归档：照常解析，只是没有元数据 ——
    const QString sV2 = work + QStringLiteral("/rooted.dat");
    FwArc::Meta metaV2;
    QString sV2Err;
    const bool bV2Inspect = FileManager::inspectArchive(sV2, metaV2, sV2Err)
                            && metaV2.version == FwArc::VERSION_V2
                            && !metaV2.entries.isEmpty()
                            && !metaV2.entries.first().hasFsMetadata;
    FileManager fmV2;
    const bool bV2Load = fmV2.QLoadMergedFile(sV2)
                         && !fmV2.getFileInfos().isEmpty()
                         && !fmV2.getFileInfos().first().bHasFsMetadata;
    report(bV2Inspect && bV2Load,
           QStringLiteral("P8: v2 fixture still loads, without metadata flag"));

    // —— v3 头部 CRC 仍然覆盖新增的元数据字段 ——
    const QString sBad = sDir + QStringLiteral("/bad_meta.fwda");
    bool bCorrupted = false;
    if (QFile::copy(sArc, sBad) && bInspect)
    {
        const qint64 nOff = qint64(FwArc::GLOBAL_HDR) + 4
                            + qint64(meta.root.toUtf8().size())
                            + 24 + 2;   // 第一条目 created 字段内部
        QFile f(sBad);
        if (f.open(QIODevice::ReadWrite) && f.seek(nOff))
        {
            char ch = 0;
            if (f.getChar(&ch))
            {
                ch = char(ch ^ 0x5A);
                bCorrupted = f.seek(nOff) && f.write(&ch, 1) == 1;
            }
        }
    }
    FileManager fmBad;
    QString sBadErr;
    const bool bBadRejected = bCorrupted && !fmBad.validateMergedFile(sBad, &sBadErr);
    report(bBadRejected, QStringLiteral("P9: corrupted v3 metadata rejected by header CRC"));

    // —— v3 归档再合并：条目里已有的元数据必须随条目一起带走 ——
    const QString sAgain = sDir + QStringLiteral("/again.fwda");
    FileManager fmAgain;
    bool bAgainDone = false, bAgainOk = false;
    QObject::connect(&fmAgain, &FileManager::operationFinished,
                     [&](bool ok, const QString &) { bAgainDone = true; bAgainOk = ok; });
    fmAgain.QMergeFiles(QVector<QString>{ sArc }, sAgain);
    FwArc::Meta metaAgain;
    QString sAgainErr;
    bool bAgainMeta = false;
    if (bAgainDone && bAgainOk && FileManager::inspectArchive(sAgain, metaAgain, sAgainErr))
    {
        for (const FwArc::Entry &e : metaAgain.entries)
        {
            if (!e.displayPath.endsWith(QStringLiteral("beta.txt"), Qt::CaseInsensitive))
                continue;
            bAgainMeta = e.hasFsMetadata
                         && near(qint64(e.modifiedMs), nModMs)
                         && (e.attributes & FwMeta::ATTR_READONLY);
        }
    }
    report(bAgainMeta,
           QStringLiteral("P10: merging an existing v3 archive keeps entry metadata"));

    QFile::remove(sBad);
    QFile::remove(sAgain);
    // 释放仍打开的归档句柄，否则 Windows 上 meta.fwda 删不掉、临时目录会残留
    fmLoad.unLoadMergedFile();
    fmV2.unLoadMergedFile();
    clearReadOnlyForCleanup(sDir);
    QDir(sDir).removeRecursively();
}

// ---------------------------------------------------------------------------
// Q: FWDA v4 条目标记
//   合并写出 v4；标记可归一化/去重/拒绝非法输入；保存后重新加载仍在；
//   展开归档再合并时标记跟随；文件列表有「标记」列且筛选框能按标记命中；
//   属性文本显示标记，标记对话框能合并已有勾选与新增输入。
// ---------------------------------------------------------------------------
static void verifyArchiveTags(const QString &work, const QString &scratchDir)
{
    Q_UNUSED(work);
    Q_UNUSED(scratchDir);
    const QString sDir = makeWritableTestDir(QStringLiteral("archive_tags"));
    report(QDir(sDir).exists(), QStringLiteral("Q0: tag test dir"));
    if (!QDir(sDir).exists())
        return;

    auto writeFile = [](const QString &sPath, const QByteArray &data) {
        QFile f(sPath);
        return f.open(QIODevice::WriteOnly | QIODevice::Truncate)
               && f.write(data) == data.size();
    };
    auto findInfo = [](const QVector<FILEINFO> &infos, const QString &sSuffix) {
        for (const FILEINFO &fi : infos)
            if (fi.sFilePath.endsWith(sSuffix, Qt::CaseInsensitive))
                return fi;
        return FILEINFO();
    };

    const QString sOne   = sDir + QStringLiteral("/one.txt");
    const QString sTwo   = sDir + QStringLiteral("/two.txt");
    const QString sThree = sDir + QStringLiteral("/three.txt");
    const bool bFiles = writeFile(sOne, QByteArray(64, '1'))
                        && writeFile(sTwo, QByteArray(64, '2'))
                        && writeFile(sThree, QByteArray(64, '3'));
    report(bFiles, QStringLiteral("Q1: tag source files ready"));
    if (!bFiles)
    {
        QDir(sDir).removeRecursively();
        return;
    }

    const QString sArc = sDir + QStringLiteral("/tags.fwda");
    FileManager fm;
    bool bMergeDone = false, bMergeOk = false;
    QObject::connect(&fm, &FileManager::operationFinished,
                     [&](bool ok, const QString &) { bMergeDone = true; bMergeOk = ok; });
    fm.QMergeFiles(QVector<QString>{ sOne, sTwo, sThree }, sArc);

    FwArc::Meta meta;
    QString sErr;
    const bool bInspect = FileManager::inspectArchive(sArc, meta, sErr);
    report(bMergeDone && bMergeOk && bInspect
               && meta.version == FwArc::VERSION_V4 && meta.entries.size() == 3,
           QStringLiteral("Q2: merge writes FWDA v4 (v=%1, n=%2, err=%3)")
               .arg(bInspect ? int(meta.version) : -1)
               .arg(bInspect ? meta.entries.size() : -1)
               .arg(sErr));

    const bool bLoaded = fm.QLoadMergedFile(sArc);
    QVector<FILEINFO> infos = fm.getFileInfos();
    const QString sPathOne   = findInfo(infos, QStringLiteral("one.txt")).sFilePath;
    const QString sPathTwo   = findInfo(infos, QStringLiteral("two.txt")).sFilePath;
    const QString sPathThree = findInfo(infos, QStringLiteral("three.txt")).sFilePath;

    QString sTagErr;
    const bool bSet = !sPathOne.isEmpty() && !sPathTwo.isEmpty()
                      && fm.setEntryTags(sPathOne,
                                         QStringList{ QStringLiteral("合同"),
                                                      QStringLiteral("重要") }, &sTagErr)
                      && fm.setEntryTags(sPathTwo, QStringList{ QStringLiteral("重要") },
                                         &sTagErr);
    infos = fm.getFileInfos();
    const FILEINFO fiOne = findInfo(infos, QStringLiteral("one.txt"));
    const FILEINFO fiTwo = findInfo(infos, QStringLiteral("two.txt"));
    report(bLoaded && bSet && fm.isDirty()
               && fiOne.listTags == QStringList{ QStringLiteral("合同"),
                                                 QStringLiteral("重要") }
               && fiTwo.listTags == QStringList{ QStringLiteral("重要") },
           QStringLiteral("Q3: setEntryTags updates entries and marks archive dirty"));

    QString sNormErr;
    const bool bNorm = !sPathThree.isEmpty()
                       && fm.setEntryTags(sPathThree,
                              QStringList{ QStringLiteral(" 合同 "), QStringLiteral("合同"),
                                           QStringLiteral("  机密  ") }, &sNormErr);
    const FILEINFO fiThree = findInfo(fm.getFileInfos(), QStringLiteral("three.txt"));
    report(bNorm
               && fiThree.listTags == QStringList{ QStringLiteral("合同"),
                                                   QStringLiteral("机密") },
           QStringLiteral("Q4: tags are trimmed/deduped before storing (%1)")
               .arg(fiThree.listTags.join(QStringLiteral("|"))));

    QString sBadErr;
    const bool bRejectSep = !fm.setEntryTags(sPathThree,
                                             QStringList{ QStringLiteral("a,b") }, &sBadErr)
                            && !sBadErr.isEmpty();
    const QString sTooLong(int(FwArc::MAX_TAG_CHARS) + 5, QLatin1Char('x'));
    const bool bRejectLong = !fm.setEntryTags(sPathThree, QStringList{ sTooLong }, &sBadErr);
    report(bRejectSep && bRejectLong,
           QStringLiteral("Q5: invalid tags (separator / too long) are rejected"));

    const QStringList allTags = fm.allTags();
    report(allTags.size() == 3
               && allTags.contains(QStringLiteral("合同"))
               && allTags.contains(QStringLiteral("重要"))
               && allTags.contains(QStringLiteral("机密")),
           QStringLiteral("Q6: allTags() lists the archive's tags once (%1)")
               .arg(allTags.join(QStringLiteral("|"))));

    const bool bSaved = fm.save();
    FileManager fmReload;
    const bool bReloaded = bSaved && fmReload.QLoadMergedFile(sArc);
    const QVector<FILEINFO> infos2 = fmReload.getFileInfos();
    const FILEINFO fiOne2 = findInfo(infos2, QStringLiteral("one.txt"));
    const FILEINFO fiThree2 = findInfo(infos2, QStringLiteral("three.txt"));
    FwArc::Meta meta2;
    const bool bHeaderOk = FileManager::inspectArchive(sArc, meta2, sErr)
                           && meta2.version == FwArc::VERSION_V4
                           && meta2.entries.size() == 3;
    bool bEntryInHeader = false;
    for (const FwArc::Entry &e : meta2.entries)
        if (e.displayPath.endsWith(QStringLiteral("one.txt"), Qt::CaseInsensitive))
            bEntryInHeader = e.tags.size() == 2;
    report(bReloaded && !fmReload.isDirty()
               && fiOne2.listTags == QStringList{ QStringLiteral("合同"),
                                                  QStringLiteral("重要") }
               && fiThree2.listTags == QStringList{ QStringLiteral("合同"),
                                                    QStringLiteral("机密") }
               && bHeaderOk && bEntryInHeader,
           QStringLiteral("Q7: tags survive save + reload and are stored in the v4 header"));

    const QString sArc2 = sDir + QStringLiteral("/merged2.fwda");
    FileManager fmMerge;
    bool bMerge2Done = false, bMerge2Ok = false;
    QObject::connect(&fmMerge, &FileManager::operationFinished,
                     [&](bool ok, const QString &) { bMerge2Done = true; bMerge2Ok = ok; });
    fmMerge.QMergeFiles(QVector<QString>{ sArc }, sArc2);
    FwArc::Meta meta3;
    bool bCarry = false;
    if (bMerge2Done && bMerge2Ok && FileManager::inspectArchive(sArc2, meta3, sErr))
    {
        for (const FwArc::Entry &e : meta3.entries)
            if (e.displayPath.endsWith(QStringLiteral("two.txt"), Qt::CaseInsensitive))
                bCarry = e.tags == QStringList{ QStringLiteral("重要") };
    }
    report(bCarry, QStringLiteral("Q8: merging a tagged archive carries entry tags"));

    bool bColumn = false, bCell = false, bFilter = false;
    {
        ProbeTreeWindow mw;
        QMetaObject::invokeMethod(&mw, "onActionViewTreeTriggered", Qt::DirectConnection);
        mw.loadArchiveFile(sArc);
        auto *pView = mw.findChild<QTreeView *>();
        auto *pModel = pView ? qobject_cast<QStandardItemModel *>(pView->model()) : nullptr;
        bColumn = pModel != nullptr && pModel->columnCount() == 4
                  && pModel->headerData(3, Qt::Horizontal).toString()
                         == QStringLiteral("标记");

        QStandardItem *pOneItem = nullptr;
        if (pModel != nullptr)
        {
            QList<QStandardItem *> stack;
            for (int r = 0; r < pModel->rowCount(); ++r)
                if (QStandardItem *p = pModel->item(r, 0))
                    stack << p;
            while (!stack.isEmpty())
            {
                QStandardItem *p = stack.takeLast();
                if (p->text().endsWith(QStringLiteral("one.txt")))
                {
                    pOneItem = p;
                    break;
                }
                for (int r = 0; r < p->rowCount(); ++r)
                    if (QStandardItem *c = p->child(r, 0))
                        stack << c;
            }
        }
        if (pOneItem != nullptr && pModel != nullptr)
        {
            const QModelIndex tagIndex =
                pModel->indexFromItem(pOneItem).siblingAtColumn(3);
            const QStandardItem *pTagsCell = pModel->itemFromIndex(tagIndex);
            bCell = pTagsCell != nullptr
                    && pTagsCell->text().contains(QStringLiteral("合同"));
        }

        auto *pFilter = mw.findChild<QLineEdit *>(QStringLiteral("leFilter"));
        if (pFilter != nullptr && pModel != nullptr)
        {
            pFilter->setText(QStringLiteral("机密"));
            QElapsedTimer tWait;
            tWait.start();
            while (tWait.elapsed() < 500)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

            std::function<int(const QStandardItem *)> countLeaves =
                [&](const QStandardItem *pParent) {
                    int n = 0;
                    for (int r = 0; r < pParent->rowCount(); ++r)
                    {
                        const QStandardItem *c = pParent->child(r, 0);
                        if (c == nullptr)
                            continue;
                        if (c->data(FileListModel::InfoRole).isValid())
                            ++n;
                        else
                            n += countLeaves(c);
                    }
                    return n;
                };
            int nVisible = 0;
            for (int r = 0; r < pModel->rowCount(); ++r)
            {
                const QStandardItem *p = pModel->item(r, 0);
                if (p == nullptr)
                    continue;
                if (p->data(FileListModel::InfoRole).isValid())
                    ++nVisible;
                else
                    nVisible += countLeaves(p);
            }
            bFilter = nVisible == 1;   // 只有 three.txt 的标记是「机密」
            pFilter->clear();
            tWait.restart();
            while (tWait.elapsed() < 300)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
    }
    report(bColumn && bCell && bFilter,
           QStringLiteral("Q9: tag column, cell text and tag-based filtering work"));

    {
        DlgEntryTags dlgTags(QStringLiteral("one.txt"),
                             QStringList{ QStringLiteral("合同"), QStringLiteral("重要") },
                             QStringList{ QStringLiteral("合同") });
        auto *pEdit = dlgTags.findChild<QLineEdit *>(QStringLiteral("leNewTag"));
        auto *pAddTag = dlgTags.findChild<QPushButton *>(QStringLiteral("btnAddTag"));
        bool bDlgOk = false;
        if (pEdit != nullptr && pAddTag != nullptr)
        {
            pEdit->setText(QStringLiteral("机密，机密"));
            pAddTag->click();
            const QStringList tags = dlgTags.tags();
            bDlgOk = tags.size() == 2 && tags.contains(QStringLiteral("合同"))
                     && tags.contains(QStringLiteral("机密"));
        }
        report(bDlgOk,
               QStringLiteral("Q10: tag dialog merges checked and typed tags without duplicates"));
    }

    // Q11: 伪造的 v4 头部（TagCount 超限）必须被拒绝。
    // 只改字段不改 CRC 会先被头部校验拦住，所以这里按格式规则重算一遍头部 CRC，
    // 确保真正走到「标记数量异常」这条分支，而不是碰巧被别的检查挡下。
    const QString sBadTags = sDir + QStringLiteral("/bad_tags.fwda");
    bool bBadHeaderReady = false;
    if (QFile::copy(sArc, sBadTags) && bHeaderOk)
    {
        QFile f(sBadTags);
        if (f.open(QIODevice::ReadWrite))
        {
            const qint64 nHeaderSize = qint64(meta2.headerSize);
            QByteArray hdr(qsizetype(nHeaderSize), Qt::Uninitialized);
            if (f.seek(0) && f.read(hdr.data(), nHeaderSize) == nHeaderSize)
            {
                // 第一条目的 TagCount 位于「全局头 + 根长度 + 根 + 固定字段末尾」
                const qint64 nTagCountPos = qint64(FwArc::GLOBAL_HDR) + 4
                                            + qint64(meta2.root.toUtf8().size())
                                            + qint64(FwArc::ENTRY_FIXED_V4) - 4;
                const quint32 nBadCount = FwArc::MAX_TAGS + 1;
                for (int k = 0; k < 4; ++k)
                    hdr[int(nTagCountPos) + k] = char((nBadCount >> (8 * k)) & 0xFF);
                FwArc::Crc32 crc;
                crc.update(hdr.constData(), 16);
                crc.update(hdr.constData() + 20, 4);
                crc.update(hdr.constData() + FwArc::GLOBAL_HDR,
                           nHeaderSize - qint64(FwArc::GLOBAL_HDR));
                const quint32 nCrc = crc.value();
                for (int k = 0; k < 4; ++k)
                    hdr[16 + k] = char((nCrc >> (8 * k)) & 0xFF);
                bBadHeaderReady = f.seek(0) && f.write(hdr) == hdr.size();
                f.close();
            }
        }
    }
    FileManager fmBadTags;
    QString sBadTagsErr;
    const bool bBadTagsRejected = bBadHeaderReady
                                  && !fmBadTags.validateMergedFile(sBadTags, &sBadTagsErr);
    report(bBadTagsRejected,
           QStringLiteral("Q11: oversized TagCount in a forged v4 header is rejected (%1)")
               .arg(sBadTagsErr));
    QFile::remove(sBadTags);

    fm.unLoadMergedFile();
    fmReload.unLoadMergedFile();
    QFile::remove(sArc2);
    QDir(sDir).removeRecursively();
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
    const QString g4Root = FwArc::commonRoot(
        QStringList{ QStringLiteral("D:/testfiles_copy/a/file1.txt"),
                     work + QStringLiteral("/basic.docx") });
    const bool bG4Root = (rooted.root == g4Root);
    const bool bG4Paths = g4Root.isEmpty()
        ? rootedPaths.contains(QStringLiteral("D/testfiles_copy/a/file1.txt"))
        : rootedPaths.contains(QStringLiteral("testfiles_copy/a/file1.txt"));
    report(bG4 && bG4Root && bG4Paths
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
    const bool bLayout = shot.width() > 100
        && page && page->isVisible()
        && page->size().width() > 100 && page->size().height() > 100
        && browser && browser->size().width() > 100;
    report(bLayout,
           QStringLiteral("A7: tab/page/browser layout %1x%2 page=%3x%4")
               .arg(shot.width()).arg(shot.height())
               .arg(page ? page->size().width() : 0)
               .arg(page ? page->size().height() : 0));

    tabs.removeTab(0);
    delete own;

    // A8: 插件相关的四个入口已合并成一个「插件设置」对话框
    QStringList sActions;
    for (QAction *a : mw.findChildren<QAction *>())
        sActions << a->text();
    report(sActions.contains(QStringLiteral("插件设置…")),
           QStringLiteral("A8: single menu action 插件设置 exists"));

    // A9: 旧的分散入口不能再长回来（诊断/管理/重载/格式各自一个菜单项）
    report(!sActions.contains(QStringLiteral("插件诊断"))
               && !sActions.contains(QStringLiteral("插件管理"))
               && !sActions.contains(QStringLiteral("重新加载插件"))
               && !sActions.contains(QStringLiteral("支持的格式…")),
           QStringLiteral("A9: legacy plugin-menu entries removed"));

    // A10: 合并后两个只读页签有内容（状态页有插件/部署信息，格式页有插件后缀）
    const QString sStatusText = mw.pluginStatusText();
    const QString sFormatsText = mw.supportedFormatsText();
    report(sStatusText.contains(QStringLiteral("已加载插件"))
               && sStatusText.contains(QStringLiteral("部署自检"))
               && sFormatsText.contains(QStringLiteral("PluginWord"))
               && sFormatsText.contains(QStringLiteral(".docx")),
           QStringLiteral("A10: plugin settings text pages populated"));

    // A11: 插件拒绝打开时的说明文案（接口 v6）。
    // 用户报过的问题：WAV 打不开时界面只说「插件无法显示该文件」，而日志里其实是
    // "没有可用的音频输出设备"——界面显示的信息是错的，照它永远修不好。
    {
        const QString sWithReason = MainWindow::pluginFailureText(
            QStringLiteral("audio.wav"), QStringLiteral("系统中没有可用的音频输出设备"));
        const QString sNoReason = MainWindow::pluginFailureText(
            QStringLiteral("audio.wav"), QStringLiteral("   "));
        report(sWithReason.contains(QStringLiteral("audio.wav"))
                   && sWithReason.contains(QStringLiteral("系统中没有可用的音频输出设备"))
                   && !sWithReason.contains(QStringLiteral("插件无法显示该文件")),
               QStringLiteral("A11a: plugin failure text carries the real reason ('%1')").arg(sWithReason));
        report(sNoReason == QStringLiteral("插件无法显示该文件：audio.wav"),
               QStringLiteral("A11b: plugins without a reason keep the generic text"));
    }
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// AUDIT-2026-11：本轮审查修复的回归测试
//   P0-1 固定 tmp/bak 名 / 跨进程锁；P1-2 二次合并来源 CRC；P1-3 删除事务；
//   P1-4 分割成功语义；P1-5 导出不覆盖；P1-6 来源变化；P1-9 一次解析复用；
//   P1-11 路径段；P1-12/P2-3 ZIP 预算与 CRC；P2-2 严格 UTF-8。
// ---------------------------------------------------------------------------
namespace {

void put16(QByteArray &a, quint16 v)
{
    a.append(char(v & 0xFF));
    a.append(char((v >> 8) & 0xFF));
}

void put32(QByteArray &a, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        a.append(char((v >> (8 * i)) & 0xFF));
}

void writeBytes(const QString &path, const QByteArray &data)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(data);
}

// 构造只有一个 stored 条目的最小 ZIP。declaredUncomp 可故意写错，
// 用于验证 ZipReader 的预算与 CRC 校验（不依赖任何外部 ZIP 工具）。
QByteArray buildStoredZip(const QByteArray &name, const QByteArray &payload,
                          quint32 declaredUncomp = 0xFFFFFFFEu,
                          bool corruptCentralCrc = false)
{
    const quint32 crc = FwArc::Crc32::of(payload);
    const quint32 uncomp = (declaredUncomp == 0xFFFFFFFEu)
                               ? quint32(payload.size()) : declaredUncomp;

    QByteArray zip;
    // local file header
    put32(zip, 0x04034b50u); put16(zip, 20); put16(zip, 0); put16(zip, 0);
    put16(zip, 0); put16(zip, 0);
    put32(zip, crc); put32(zip, quint32(payload.size())); put32(zip, uncomp);
    put16(zip, quint16(name.size())); put16(zip, 0);
    zip.append(name);
    zip.append(payload);
    const quint32 cdOff = quint32(zip.size());
    // central directory
    put32(zip, 0x02014b50u); put16(zip, 20); put16(zip, 20); put16(zip, 0); put16(zip, 0);
    put16(zip, 0); put16(zip, 0);
    put32(zip, corruptCentralCrc ? (crc ^ 0xFFFFFFFFu) : crc);
    put32(zip, quint32(payload.size())); put32(zip, uncomp);
    put16(zip, quint16(name.size())); put16(zip, 0); put16(zip, 0);
    put16(zip, 0); put16(zip, 0); put32(zip, 0); put32(zip, 0);
    zip.append(name);
    const quint32 cdSize = quint32(zip.size()) - cdOff;
    // EOCD（单磁盘）
    put32(zip, 0x06054b50u); put16(zip, 0); put16(zip, 0); put16(zip, 1); put16(zip, 1);
    put32(zip, cdSize); put32(zip, cdOff); put16(zip, 0);
    return zip;
}

} // namespace

static void verifyAudit2026Fixes(const QString &work, const QString &scratchDir)
{
    Q_UNUSED(work);

    // ---- A1: 严格 UTF-8（P2-2）----
    {
        auto okBytes = [](std::initializer_list<int> bytes) {
            QByteArray b;
            for (int v : bytes) b.append(char(v));
            return FwArc::isValidUtf8(b);
        };
        const bool bValidBasic = FwArc::isValidUtf8(QByteArray("hello, \xE4\xB8\xAD\xE6\x96\x87"));
        const bool bValidReplacement = FwArc::isValidUtf8(QByteArray("\xEF\xBF\xBD"));
        const bool bValidMax = okBytes({0xF4, 0x8F, 0xBF, 0xBF});
        const bool bOverlong2 = okBytes({0xC0, 0xAF});
        const bool bOverlong3 = okBytes({0xE0, 0x80, 0xAF});
        const bool bOverlong4 = okBytes({0xF0, 0x80, 0x80, 0xAF});
        const bool bSurrogate = okBytes({0xED, 0xA0, 0x80});
        const bool bTooBig = okBytes({0xF4, 0x90, 0x80, 0x80});
        const bool bTruncated = okBytes({0xE2, 0x82});
        report(bValidBasic && bValidReplacement && bValidMax
               && !bOverlong2 && !bOverlong3 && !bOverlong4
               && !bSurrogate && !bTooBig && !bTruncated,
               QStringLiteral("AUDIT-P2-2: strict UTF-8 rejects overlong/surrogate/>U+10FFFF"));
    }

    // ---- A2: 路径段安全（P1-11）----
    {
        const bool bTrailingDot = !FwArc::isSafeRelativePath(QStringLiteral("a/b."));
        const bool bTrailingSpace = !FwArc::isSafeRelativePath(QStringLiteral("a/b "));
        const bool bMidDevice = !FwArc::isSafeRelativePath(QStringLiteral("CON/x.txt"));
        const bool bDevWithExt = !FwArc::isSafeRelativePath(QStringLiteral("a/COM1.txt"));
        const bool bLongSeg = !FwArc::isSafeRelativePath(QString(260, QLatin1Char('a')));
        const bool bGood = FwArc::isSafeRelativePath(QStringLiteral("dir/sub/file.txt"));
        report(bTrailingDot && bTrailingSpace && bMidDevice && bDevWithExt && bLongSeg && bGood,
               QStringLiteral("AUDIT-P1-11: path segments reject dots/spaces/device/long names"));
    }

    // ---- A3: 合并不得删除输入/无关的固定辅助名文件（P0-1）----
    {
        const QString out = scratchDir + QStringLiteral("/audit_fixed_aux.dat");
        const QString in  = out + QStringLiteral(".tmp.fwda");   // 旧实现会误删它
        writeBytes(in, "keep-me");
        bool bDone = false, bOk = false;
        QString sMsg;
        FileManager fm;
        QObject::connect(&fm, &FileManager::operationFinished,
                         [&](bool o, const QString &m) { bDone = true; bOk = o; sMsg = m; });
        fm.QMergeFiles(QVector<QString>{in}, out);
        const bool bInputKept = QFileInfo::exists(in) && readFile(in) == QByteArray("keep-me");
        FwArc::Meta m;
        QString e;
        const bool bOutOk = FileManager::inspectArchive(out, m, e) && m.entries.size() == 1;
        report(bDone && bOk && bInputKept && bOutOk,
               QStringLiteral("AUDIT-P0-1: merge keeps input named like old fixed tmp (%1)").arg(sMsg));
    }

    // ---- A4: 同一输出的写锁（P0-1）----
    {
        const QString out = scratchDir + QStringLiteral("/audit_lock.dat");
        const QString in  = scratchDir + QStringLiteral("/audit_lock.in");
        writeBytes(in, "lock");
        // 与 FileManager 的锁格式一致：<输出>.fwdalock，NewOnly 创建后写 PID
        const QString lockPath = out + QStringLiteral(".fwdalock");
        QFile held(lockPath);
        const bool bHeld = held.open(QIODevice::WriteOnly | QIODevice::NewOnly);
        held.write(QByteArray::number(qint64(QCoreApplication::applicationPid())));
        held.close();

        auto runMerge = [&](bool *pbDone, bool *pbOk, QString *psMsg) {
            *pbDone = false; *pbOk = true; psMsg->clear();
            FileManager fm;
            QObject::connect(&fm, &FileManager::operationFinished,
                             [=](bool o, const QString &m) { *pbDone = true; *pbOk = o; *psMsg = m; });
            fm.QMergeFiles(QVector<QString>{in}, out);
        };

        bool bDone = false, bOk = true;
        QString sMsg;
        runMerge(&bDone, &bOk, &sMsg);
        const bool bFreshRejected = bHeld && bDone && !bOk && sMsg.contains(QStringLiteral("实例"));

        // 锁龄超过 30 秒，但 PID 仍是当前存活进程：不能把活锁当崩溃残留删掉。
        // 旧实现只看 mtime，长保存（几 GB）会被第二个实例抢锁。
        QFile ageLock(lockPath);
        const bool bAged = ageLock.open(QIODevice::ReadWrite);
        if (bAged)
            ageLock.setFileTime(QDateTime::currentDateTime().addSecs(-40),
                                QFileDevice::FileModificationTime);
        ageLock.close();
        bool bDone2 = false, bOk2 = true;
        QString sMsg2;
        runMerge(&bDone2, &bOk2, &sMsg2);
        const bool bAliveStillRejected = bAged && bDone2 && !bOk2
                                         && sMsg2.contains(QStringLiteral("实例"));

        // 换成“已退出进程”的 PID + 陈旧时间：才允许清理并继续。
        QFile deadLock(lockPath);
        const bool bDeadWritten = deadLock.open(QIODevice::WriteOnly | QIODevice::Truncate);
        if (bDeadWritten)
        {
            deadLock.write("2147483647");
            deadLock.close();
        }
        QFile deadAge(lockPath);
        if (deadAge.open(QIODevice::ReadWrite))
            deadAge.setFileTime(QDateTime::currentDateTime().addSecs(-40),
                                QFileDevice::FileModificationTime);
        deadAge.close();
        bool bDone3 = false, bOk3 = false;
        QString sMsg3;
        runMerge(&bDone3, &bOk3, &sMsg3);
        QFile::remove(lockPath);
        const bool bStaleReclaimed = bDeadWritten && bDone3 && bOk3
                                     && QFileInfo::exists(out);

        report(bFreshRejected && bAliveStillRejected && bStaleReclaimed,
               QStringLiteral("AUDIT-P0-1: lock rejects live writer even when old; reclaims dead stale lock"));
    }

    // ---- A5: 二次合并必须校验来源内容 CRC（P1-2）----
    {
        const QString src = scratchDir + QStringLiteral("/audit_crc_src.bin");
        const QString arc = scratchDir + QStringLiteral("/audit_crc.dat");
        const QString out = scratchDir + QStringLiteral("/audit_crc_out.dat");
        QFile::remove(out);
        writeBytes(src, QByteArray(4096, 'A'));
        FileManager fm;
        fm.QMergeFiles(QVector<QString>{src}, arc);
        FwArc::Meta meta;
        QString err;
        const bool bMeta = FileManager::inspectArchive(arc, meta, err) && meta.entries.size() == 1;
        if (bMeta)
        {
            QFile f(arc);
            if (f.open(QIODevice::ReadWrite))
            {
                f.seek(qint64(meta.entries.first().contentPos));
                const char c = 'B';
                f.write(&c, 1);
            }
        }
        bool bDone = false, bOk = true;
        QString sMsg;
        FileManager fm2;
        QObject::connect(&fm2, &FileManager::operationFinished,
                         [&](bool o, const QString &m) { bDone = true; bOk = o; sMsg = m; });
        fm2.QMergeFiles(QVector<QString>{arc}, out);
        report(bMeta && bDone && !bOk && !QFileInfo::exists(out)
               && sMsg.contains(QStringLiteral("CRC")),
               QStringLiteral("AUDIT-P1-2: merge rejects corrupted source content (%1)").arg(sMsg));
    }

    // ---- A6: 删除后保存走完整事务重写（P1-3）----
    {
        const QString a = scratchDir + QStringLiteral("/audit_del_a.txt");
        const QString b = scratchDir + QStringLiteral("/audit_del_b.txt");
        const QString arc = scratchDir + QStringLiteral("/audit_del.dat");
        writeBytes(a, "aaa");
        writeBytes(b, "bbb");
        FileManager fm;
        fm.QMergeFiles(QVector<QString>{a, b}, arc);
        FileManager fm2;
        const bool bLoaded = fm2.QLoadMergedFile(arc);
        const QString keep = fm2.getFileInfos().first().sFilePath;
        const bool bDel = fm2.deleteFile(keep);
        const bool bSaved = fm2.save();
        FwArc::Meta m;
        QString e;
        const bool bMetaOk = FileManager::inspectArchive(arc, m, e)
                             && m.entries.size() == 1 && m.fileCount == 1;
        report(bLoaded && bDel && bSaved && bMetaOk,
               QStringLiteral("AUDIT-P1-3: delete+save rewrites with %1 slots").arg(m.fileCount));
    }

    // ---- A7: 新增文件在保存前被修改必须失败（P1-6）----
    {
        const QString base = scratchDir + QStringLiteral("/audit_pending_base.txt");
        const QString add  = scratchDir + QStringLiteral("/audit_pending_add.txt");
        const QString arc  = scratchDir + QStringLiteral("/audit_pending.dat");
        writeBytes(base, "base");
        writeBytes(add, "first");
        FileManager fm;
        fm.QMergeFiles(QVector<QString>{base}, arc);
        FileManager fm2;
        const bool bLoaded = fm2.QLoadMergedFile(arc);
        fm2.addFiles(QVector<QString>{add});
        {
            QFile f(add);
            if (f.open(QIODevice::Append))
                f.write("MORE");
        }
        const bool bSaved = fm2.save();
        const QString sErr = fm2.lastError();
        FwArc::Meta m;
        QString e;
        const bool bStillOld = FileManager::inspectArchive(arc, m, e) && m.entries.size() == 1;
        report(bLoaded && !bSaved && sErr.contains(QStringLiteral("变化")) && bStillOld,
               QStringLiteral("AUDIT-P1-6: changed pending file aborts save (%1)").arg(sErr));
    }

    // ---- A8: 导出绝不覆盖/截断已有文件（P1-5）----
    {
        const QString src = scratchDir + QStringLiteral("/dup.txt");
        const QString arc = scratchDir + QStringLiteral("/audit_export.dat");
        writeBytes(src, "export-once");
        FileManager fm;
        fm.QMergeFiles(QVector<QString>{src}, arc);
        FileManager fm2;
        fm2.QLoadMergedFile(arc);
        const QString name = fm2.getFileInfos().first().sFilePath;
        fm2.outputFile(name, scratchDir);
        const QString first = fm2.lastOutputPath();
        fm2.outputFile(name, scratchDir);
        const QString second = fm2.lastOutputPath();
        const bool bOk = !second.isEmpty() && second != first
                         && QFileInfo::exists(first)
                         && readFile(first) == QByteArray("export-once")
                         && readFile(second) == QByteArray("export-once");
        report(bOk, QStringLiteral("AUDIT-P1-5: export renames instead of truncating (%1 | %2)")
                        .arg(first, second));
    }

    // ---- A9: 校验结果可直接复用于加载（P1-9）----
    {
        const QString arc = scratchDir + QStringLiteral("/audit_preparse.dat");
        const QString src = scratchDir + QStringLiteral("/audit_preparse.txt");
        writeBytes(src, "preparse");
        FileManager fm;
        fm.QMergeFiles(QVector<QString>{src}, arc);
        FileManager fm2;
        FwArc::Meta meta;
        QString err;
        const bool bValid = fm2.validateMergedFile(arc, &meta, &err);
        const bool bLoaded = fm2.QLoadMergedFile(arc, &meta);
        report(bValid && bLoaded && fm2.isArchiveLoaded(),
               QStringLiteral("AUDIT-P1-9: validated meta reused by load"));
    }

    // ---- A10: 分割完全成功 = 全部条目都输出（P1-4）----
    {
        const QString a = scratchDir + QStringLiteral("/audit_split_a.txt");
        const QString b = scratchDir + QStringLiteral("/audit_split_b.txt");
        const QString arc = scratchDir + QStringLiteral("/audit_split.dat");
        const QString outDir = scratchDir + QStringLiteral("/audit_split_out");
        QDir(outDir).removeRecursively();
        writeBytes(a, "split-a");
        writeBytes(b, "split-b");
        FileManager fm;
        fm.QMergeFiles(QVector<QString>{a, b}, arc);
        bool bDone = false, bOk = false;
        QString sMsg;
        FileManager fm2;
        QObject::connect(&fm2, &FileManager::operationFinished,
                         [&](bool o, const QString &m) { bDone = true; bOk = o; sMsg = m; });
        fm2.QSplitFiles(arc, false, outDir);
        const bool bFiles = QFileInfo::exists(outDir + QStringLiteral("/audit_split_a.txt"))
                            && QFileInfo::exists(outDir + QStringLiteral("/audit_split_b.txt"));
        report(bDone && bOk && bFiles,
               QStringLiteral("AUDIT-P1-4: split reports success only when all entries output (%1)")
                   .arg(sMsg));
    }

    // ---- A11: ZipReader 预算与 CRC（P1-12 / P2-3）----
    {
        const QByteArray name("a.txt");
        const QByteArray payload("hello zip");
        ZipReader good;
        const bool bOpen = good.open(buildStoredZip(name, payload));
        const bool bGood = bOpen
                           && good.entry(QString::fromLatin1(name)) == payload;

        ZipReader badCrc;
        const bool bOpenBad = badCrc.open(buildStoredZip(name, payload, 0xFFFFFFFEu, true));
        const QByteArray badData = badCrc.entry(QString::fromLatin1(name));
        const bool bRejectedCrc = bOpenBad && badData.isEmpty()
                                  && badCrc.lastError().contains(QStringLiteral("CRC"));

        ZipReader overBudget;
        const bool bOpenBig = overBudget.open(
            buildStoredZip(name, payload, 100u * 1024u * 1024u));   // 声明 100 MiB（>64 MiB）
        const QByteArray bigData = overBudget.entry(QString::fromLatin1(name));
        const bool bRejectedBudget = bOpenBig && bigData.isEmpty()
                                     && overBudget.budget().exhausted;

        // 声明 0 字节却带压缩数据：不能解出内容，更不能按 0 字节计入预算。
        ZipReader zeroDeclared;
        const bool bOpenZero = zeroDeclared.open(buildStoredZip(name, payload, 0u));
        const QByteArray zeroData = zeroDeclared.entry(QString::fromLatin1(name));
        const bool bRejectedZero = bOpenZero && zeroData.isEmpty()
                                   && zeroDeclared.budget().totalBytes == 0;

        report(bGood && bRejectedCrc && bRejectedBudget && bRejectedZero,
               QStringLiteral("AUDIT-P1-12/P2-3: ZipReader verifies CRC, budget and zero-length lie"));
    }

    // ---- A12: XML 属性放大（QDom 的二次方属性插入）----
    {
        QByteArray oneTag("<r");
        for (int i = 0; i < 5000; ++i)
            oneTag += " a" + QByteArray::number(i) + "=\"v\"";
        oneTag += "/>";
        const bool bRejected = !xmlWithinTagBudget(oneTag);
        const bool bNormal = xmlWithinTagBudget(
            QByteArray("<r a=\"1\" b='2'><c/></r>"));
        report(bRejected && bNormal,
               QStringLiteral("AUDIT-XMLGUARD: per-element attribute bomb rejected, normal XML accepted"));
    }
}

// D6 的独立子进程：加载坏插件→换成真插件→重试→进程退出后模块自然卸载，
// 父进程才能删除 ZZBroken.dll，避免同一进程留下第 9 个重复模块。
static int runBrokenRetryChild()
{
    const QString childSettings = makeWritableTestDir(QStringLiteral("d6_settings"));
    if (!QDir(childSettings).exists()) return 2;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, childSettings);
    const QString dir = QCoreApplication::applicationDirPath() + QStringLiteral("/plugins");
    const QString dll = dir + QStringLiteral("/ZZBroken.dll");
    const QString json = dir + QStringLiteral("/ZZBroken.json");
    QFile::remove(dll);
    QFile::remove(json);
    {
        QFile f(dll);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 1;
        f.write("not a plugin");
    }
    {
        QFile f(json);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write("{\"Name\":\"zzBrokenChild\",\"apiVersion\":3,\"extensions\":[\"zzc\"]}");
    }

    PluginManager *pm = PluginManager::getInstance();
    pm->loadAllPlugins();
    const bool bRecorded = !pm->failureFor(dll).isEmpty();

    if (!QFile::remove(dll)) return 3;
    if (!QFile::copy(dir + QStringLiteral("/PluginPdf.dll"), dll)) return 4;
    const int nRecovered = pm->retryFailedPlugins();
    const bool bOk = bRecorded && nRecovered == 1
                     && pm->getInterface(dll) != nullptr
                     && pm->failureFor(dll).isEmpty();
    return bOk ? 0 : 1;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    for (int i = 1; i < argc; ++i)
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String("--d6-child"))
            return runBrokenRetryChild();

    // 测试必须与用户真实配置隔离：插件启停（QSettings PluginEnabled）与界面状态
    // 都会读写 ini，若用默认路径，一次测试就会改动用户设置。
    // 这里把 UserScope 的 ini 目录指到临时目录（必须在任何 QSettings 构造之前调用）。
    //
    // 受限环境下 QTemporaryDir 的子目录可能不可写（任务沙箱/只读配置），
    // 允许用 FW_TEST_SETTINGS_DIR / FW_TEST_SCRATCH_DIR 显式指定可写目录。
    const QString sSettingsOverride = qEnvironmentVariable("FW_TEST_SETTINGS_DIR");
    QTemporaryDir settingsDir;
    QString settingsPath = sSettingsOverride;
    if (settingsPath.isEmpty() && settingsDir.isValid())
        settingsPath = settingsDir.path();
    if (!settingsPath.isEmpty())
    {
        QDir().mkpath(settingsPath);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsPath);
        note(QStringLiteral("settings isolated to: ") + settingsPath);
    }
    else
    {
        // 隔离失败会让测试写进用户真实配置：必须显式失败并停止，不能继续。
        report(false, QStringLiteral("settings isolation failed (QTemporaryDir invalid)"));
        return 1;
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
    QString scratchDir = qEnvironmentVariable("FW_TEST_SCRATCH_DIR");
    if (scratchDir.isEmpty())
        scratchDir = scratch.isValid() ? scratch.path()
                                       : QDir::tempPath() + QStringLiteral("/plugin_tests");
    if (!QDir().exists(scratchDir))
        QDir().mkpath(scratchDir);
    note(QStringLiteral("scratch: ") + scratchDir);

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

    // 只用产品路径同一套读取接口（entryContent）；旧的 char** 接口已删除，
    // 避免"主程序一套校验、测试另一套"的语义漂移。
    const QByteArray basicData = fm.entryContent(basicEntry);
    const qint64 expectLen = QFileInfo(work + QStringLiteral("/basic.docx")).size();
    report(!basicData.isEmpty() && basicData.size() == expectLen,
           QStringLiteral("W12: read back %1 bytes (file %2)")
               .arg(basicData.size()).arg(expectLen));
    if (basicData.isEmpty()) return 1;

    // ---------- W5/W6: 真实插件 DLL ----------
    // 必须从 <exe>/plugins 加载（与 PluginManager、V 组同一路径），
    // 不要用 -plugins 参数指向的构建目录副本：Windows 会当成两个模块双映射，
    // 正是项目已记录的退出期虚表悬空崩溃模式。
    const QString sRuntimePlugins =
        QCoreApplication::applicationDirPath() + QStringLiteral("/plugins");
    QPluginLoader loader(sRuntimePlugins + QStringLiteral("/PluginWord.dll"));
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
        QByteArray data = basicData;              // sendFileData 需要可写缓冲区
        own->sendFileData(data.data(), data.size());
        verifyBasicDoc(own->getPluginWidget());
        verifyZoom(own->getPluginWidget());
        delete own;
    }

    // ---------- C ----------
    {
        const QByteArray chineseData = fm.entryContent(chineseEntry);
        PluginInterface *own = tmpl->createInstance();
        own->setSourceName(QStringLiteral("chinese.docx"));
        QByteArray data = chineseData;
        own->sendFileData(data.data(), data.size());
        verifyChineseDoc(own->getPluginWidget());
        delete own;
    }

    // ---------- N / M ----------
    verifyErrorPaths(tmpl, work);
    verifyInstances(tmpl, work);

    // ---------- Y: C2 数据通道 ----------
    verifyFileChannel(tmpl, work);

    // ---------- V: 其余查看器渲染冒烟 ----------
    if (qEnvironmentVariableIsEmpty("FW_SKIP_VIEWER_SMOKE"))
    {
        verifyOtherViewers(work);
    }
    else
    {
        g_skipped += 16;   // V1..V14 共 16 条 report
        note(QStringLiteral("[SKIP] whole V group skipped by FW_SKIP_VIEWER_SMOKE (16 assertions)"));
    }

    // ---------- Z: 格式识别（后缀不可靠时的兜底） ----------
    verifyFormatResolver();
    verifyUiTypeHelpers();

    // ---------- S: 截图（可选，--shots <dir>） ----------
    if (shotsDir.isEmpty())
        g_skipped += 3;   // 3 张截图未启用，按断言数计入 SKIP
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

    // ---------- K: 保存路径规划（P0：添加文件后不得拍平目录） ----------
    verifySavePathPreservation(work, scratchDir);

    // ---------- P: v3/v4 元数据与标记（合并保存 / 导出恢复 / 旧归档兼容） ----------
    verifyArchiveMetadata(work, scratchDir);
    verifyArchiveTags(work, scratchDir);

    // ---------- AUDIT-2026-11: 本轮审查修复的回归 ----------
    verifyAudit2026Fixes(work, scratchDir);

    // ---------- D: 部署自检 + 插件失败可见性 ----------
    // 放在 A 之后：D6「补文件后重试恢复」会额外加载一份插件副本，从而在
    // 后缀注册表里制造冲突告警；放到最后就不会影响前面的任何断言。
    // （D5 需要的"首次扫描记录失败"已在 W1 构造 MainWindow 时发生。）
    verifyDeploymentDiagnostics(plug);

    // ---------- X: B2 / B4 / C5（最后执行：C5 会卸载插件 DLL） ----------
    verifyCreateInstanceContract(tmpl);
    verifyDiskSpaceCheck();
    verifyPluginToggleAndReload();

    // H10/H13 的 PID 临时目录收尾清理
    QDir(QDir::tempPath() + QStringLiteral("/fw_tree_sort_test_%1")
             .arg(QCoreApplication::applicationPid())).removeRecursively();
    QDir(QDir::tempPath() + QStringLiteral("/fw_chain_test_%1")
             .arg(QCoreApplication::applicationPid())).removeRecursively();

    // ---------- 汇总 ----------
    out(QStringLiteral("---------------------------------------------"));
    out(QStringLiteral("RESULT: %1 PASS, %2 FAIL, %3 SKIP").arg(g_pass).arg(g_fail).arg(g_skipped));

    QFile logFile(logPath);
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        logFile.write(g_log.join(QLatin1Char('\n')).toUtf8());
        logFile.write("\n");
    }
    return g_fail == 0 ? 0 : 1;
}

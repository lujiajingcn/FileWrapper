#include <QApplication>
#include <QByteArray>
#include <QBuffer>
#include <QTextBrowser>
#include <QMetaObject>
#include <QDebug>
#include <QLayout>
#include <QImage>
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QFileInfo>

#include <QtGui/private/qzipwriter_p.h>

#include "mainwindow.h"

// 结果同时写文件 + stderr，规避无头环境下 qDebug 被吞的问题
static QFile   g_logFile;
static QTextStream g_log;

static void logLine(const QString &s)
{
    g_log << s << "\n";
    g_log.flush();
    fprintf(stderr, "%s\n", s.toUtf8().constData());
    fflush(stderr);
}

// ---------- 测试基础设施 ----------
static int g_fail = 0;
static int g_pass = 0;

static bool check(const QString &name, bool cond)
{
    if (cond) { g_pass++; logLine("[PASS] " + name); }
    else      { g_fail++; logLine("[FAIL] " + name); }
    return cond;
}

// 从 MainWindow 的 centralWidget 布局里取出 QTextBrowser（即 BookBrowser）
static QTextBrowser *getBrowser(MainWindow *w)
{
    QWidget *cw = w->centralWidget();
    if (!cw || !cw->layout()) return nullptr;
    for (int i = 0; i < cw->layout()->count(); ++i)
    {
        QLayoutItem *it = cw->layout()->itemAt(i);
        if (it && it->widget())
        {
            QTextBrowser *tb = qobject_cast<QTextBrowser *>(it->widget());
            if (tb) return tb;
        }
    }
    return nullptr;
}

// ---------- 样本构造 ----------
static QByteArray makePng()
{
    QImage img(16, 16, QImage::Format_ARGB32);
    img.fill(Qt::red);
    QByteArray png;
    QBuffer pb(&png);
    pb.open(QIODevice::WriteOnly);
    img.save(&pb, "PNG");
    return png;
}

static QByteArray makeEpub()
{
    const QByteArray mimetype = "application/epub+zip";
    const QByteArray container =
        "<?xml version=\"1.0\"?>\n"
        "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
        "  <rootfiles>\n"
        "    <rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>\n"
        "  </rootfiles>\n"
        "</container>\n";
    const QByteArray opf =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\">\n"
        "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
        "    <dc:title>Test Book Title</dc:title>\n"
        "  </metadata>\n"
        "  <manifest>\n"
        "    <item id=\"c1\" href=\"chap1.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
        "    <item id=\"c2\" href=\"chap2.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
        "    <item id=\"cv\" href=\"cover.png\"  media-type=\"image/png\"/>\n"
        "  </manifest>\n"
        "  <spine>\n"
        "    <itemref idref=\"c1\"/>\n"
        "    <itemref idref=\"c2\"/>\n"
        "  </spine>\n"
        "</package>\n";
    const QByteArray chap1 =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Chap1</title></head>\n"
        "<body><h1>ALICE_CHAP1_HEADING</h1>\n"
        "<p>ALICE_CHAP1_PARAGRAPH content here.</p>\n"
        "<img src=\"cover.png\" alt=\"cover\"/></body></html>\n";
    const QByteArray chap2 =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Chap2</title></head>\n"
        "<body><h1>ALICE_CHAP2_HEADING</h1>\n"
        "<p>ALICE_CHAP2_PARAGRAPH content here.</p></body></html>\n";

    QByteArray png = makePng();

    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    QZipWriter writer(&buf);
    writer.addFile("mimetype", mimetype);
    writer.addFile("META-INF/container.xml", container);
    writer.addFile("OEBPS/content.opf", opf);
    writer.addFile("OEBPS/chap1.xhtml", chap1);
    writer.addFile("OEBPS/chap2.xhtml", chap2);
    writer.addFile("OEBPS/cover.png", png);
    writer.close();
    buf.close();
    return out;
}

static QByteArray makeFb2()
{
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<FictionBook xmlns=\"http://www.gribuser.ru/xml/fictionbook/2.0\">\n"
           "  <description><title-info><book-title>FB2 Book Title</book-title></title-info></description>\n"
           "  <body>\n"
           "    <p>FB2_PARAGRAPH_ONE text.</p>\n"
           "    <p>FB2_PARAGRAPH_TWO text.</p>\n"
           "  </body>\n"
           "</FictionBook>\n";
}

static QByteArray makeMobi()
{
    // 读取 libmobi 仓库自带真实样本，端到端验证 loadMobi 经 libmobi 解析正文。
    // 路径由 __FILE__ 推导到 PluginBook 目录，避免硬编码工作区绝对路径。
    QString srcPath = QString::fromUtf8(__FILE__);
    srcPath.replace('\\', '/');
    QFileInfo fi(srcPath);
    QString pluginBookDir = fi.absolutePath() + "/..";          // .../PluginBook
    QString sample = QDir(pluginBookDir).absoluteFilePath(
        "thirdparty/libmobi/tests/samples/sample-unicode-huffdic.mobi");
    QFile f(sample);
    if (!f.open(QIODevice::ReadOnly))
    {
        logLine("[WARN] 找不到样本 MOBI: " + sample);
        return QByteArray();
    }
    QByteArray data = f.readAll();
    f.close();
    logLine(QString("[样本] 真实 MOBI 大小 = %1 字节").arg(data.size()));
    return data;
}

static QByteArray makeFakeMobi()
{
    // 损坏的 MOBI：仅有魔数、无真实结构，用于验证 libmobi 解析失败时优雅提示而非崩溃
    QByteArray b(100, 0);
    b.replace(60, 4, "MOBI");   // PalmDOC 头偏移 60 的 MOBI 魔数
    return b;
}

static QByteArray makeGarbage()
{
    QByteArray b(50, 0);        // 非 zip、非 xml、长度 < 64（跳过 mobi 判定）
    return b;
}

// ---------- 主测试 ----------
int main(int argc, char *argv[])
{
    g_logFile.setFileName("C:/Users/lujiajing/AppData/Local/Temp/booktest_result.txt");
    g_logFile.open(QIODevice::WriteOnly | QIODevice::Text);
    g_log.setDevice(&g_logFile);

    logLine("=== PluginBook 最小自测开始 ===");

    qputenv("QT_QPA_PLATFORM", "offscreen");   // 无显示器也能跑 Qt Widgets
    QApplication app(argc, argv);

    // ===== 1) EPUB =====
    {
        QByteArray epub = makeEpub();
        logLine(QString("[样本] EPUB 大小 = %1 字节").arg(epub.size()));
        MainWindow w;
        w.showBook(epub.data(), epub.size());
        QTextBrowser *b = getBrowser(&w);
        check("EPUB: 浏览器控件已创建", b != nullptr);
        if (b)
        {
            QString t = b->toPlainText();
            check("EPUB: 第1章标题渲染", t.contains("ALICE_CHAP1_HEADING"));
            check("EPUB: 第1章正文渲染", t.contains("ALICE_CHAP1_PARAGRAPH"));
            check("EPUB: 第1章含封面图片标签", b->toHtml().contains("cover.png"));

            QMetaObject::invokeMethod(&w, "on_actionNextChapter_triggered");
            QString t2 = b->toPlainText();
            check("EPUB: 翻章后显示第2章", t2.contains("ALICE_CHAP2_HEADING"));
            check("EPUB: 翻章后不再显示第1章", !t2.contains("ALICE_CHAP1_HEADING"));
        }
    }

    // ===== 2) FB2 =====
    {
        QByteArray fb2 = makeFb2();
        MainWindow w;
        w.showBook(fb2.data(), fb2.size());
        QTextBrowser *b = getBrowser(&w);
        check("FB2: 浏览器控件已创建", b != nullptr);
        if (b)
        {
            QString t = b->toPlainText();
            check("FB2: 段落一渲染", t.contains("FB2_PARAGRAPH_ONE"));
            check("FB2: 段落二渲染", t.contains("FB2_PARAGRAPH_TWO"));
        }
        // 书名解析后显示在工具栏信息标签（与 EPUB 一致），不在正文里
        QString infoText;
        QList<QLabel *> labels = w.findChildren<QLabel *>();
        for (QLabel *l : labels)
            if (l->text().contains("FB2 Book Title")) { infoText = l->text(); break; }
        check("FB2: 书名解析并显示在信息栏", !infoText.isEmpty());
    }

    // ===== 3) 真实 MOBI（libmobi 解析正文）=====
    {
        QByteArray mobi = makeMobi();
        if (mobi.isEmpty())
        {
            check("MOBI: 样本文件存在", false);
        }
        else
        {
            MainWindow w;
            w.showBook(mobi.data(), mobi.size());
            QTextBrowser *b = getBrowser(&w);
            check("MOBI: 浏览器控件已创建", b != nullptr);
            if (b)
            {
                QString t = b->toPlainText();
                QString h = b->toHtml();
                check("MOBI: libmobi 解析出正文（含书名 Libmobi）",
                      t.contains("Libmobi") || h.contains("Libmobi"));
                check("MOBI: 正文含样例句子",
                      t.contains("This is a sample for testing libmobi project."));
                check("MOBI: 未显示不支持提示",
                      !t.contains("暂不支持") && !t.contains("无法识别"));
                // 书名应显示在工具栏信息栏（与 EPUB/FB2 一致）
                QString infoText;
                QList<QLabel *> labels = w.findChildren<QLabel *>();
                for (QLabel *l : labels)
                    if (l->text().contains("Libmobi")) { infoText = l->text(); break; }
                check("MOBI: 书名显示在信息栏", !infoText.isEmpty());
            }
        }
    }

    // ===== 3b) 损坏 MOBI（libmobi 解析失败应优雅提示，而非崩溃）=====
    {
        QByteArray fake = makeFakeMobi();
        MainWindow w;
        w.showBook(fake.data(), fake.size());
        QTextBrowser *b = getBrowser(&w);
        check("MOBI(损坏): 浏览器控件已创建", b != nullptr);
        if (b)
        {
            QString t = b->toPlainText();
            check("MOBI(损坏): 提示解析失败而非崩溃", t.contains("解析电子书失败"));
        }
    }

    // ===== 4) 垃圾数据（应提示无法识别）=====
    {
        QByteArray gar = makeGarbage();
        MainWindow w;
        w.showBook(gar.data(), gar.size());
        QTextBrowser *b = getBrowser(&w);
        check("GARBAGE: 浏览器控件已创建", b != nullptr);
        if (b)
        {
            QString t = b->toPlainText();
            check("GARBAGE: 提示无法识别", t.contains("无法识别") || t.contains("不支持"));
        }
    }

    logLine(QString("==== 自测结果: %1 通过, %2 失败 ====").arg(g_pass).arg(g_fail));
    g_log.flush();
    g_logFile.close();
    return g_fail == 0 ? 0 : 1;
}

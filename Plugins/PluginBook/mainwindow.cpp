#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "zipreader.h"
#include <QLayout>
#include <QDomDocument>
#include <QRegularExpression>
#include <QImage>
#include <QUrl>
#include <QLabel>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <cstdlib>   // ::free（libmobi 返回的标题需 free）

#ifdef WITH_LIBMOBI
// libmobi 头自带 extern "C" 守卫，直接包含即可
#include "mobi.h"
#endif

// ============================================================
// 路径 / 字符串辅助（文件作用域）
// ============================================================
static QString joinPath(const QString &base, const QString &rel)
{
    QStringList parts    = base.split('/', QString::SkipEmptyParts);
    QStringList relParts = rel.split('/', QString::SkipEmptyParts);
    for (const QString &r : relParts)
    {
        if (r == ".")  continue;
        else if (r == "..") { if (!parts.isEmpty()) parts.removeLast(); }
        else parts.append(r);
    }
    return parts.join('/');
}

static QString dirOf(const QString &path)
{
    int i = path.lastIndexOf('/');
    return (i < 0) ? QString() : path.left(i + 1); // 含结尾 '/'
}

static QString escapeText(const QString &s)
{
    QString r = s;
    r.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;");
    return r;
}

// ============================================================
// BookBrowser：从内嵌数据加载图片
// ============================================================
BookBrowser::BookBrowser(QWidget *parent) : QTextBrowser(parent) {}

QVariant BookBrowser::loadResource(int type, const QUrl &name)
{
    if (type == QTextDocument::ImageResource)
    {
        QString p = name.toString();
        int i = p.indexOf("://");
        if (i >= 0) p = p.mid(i + 3);          // 去掉 zip:// / file:// 等 scheme
        while (p.startsWith('/')) p.remove(0, 1);

        // 1) 直接按 zip 内路径查找
        if (m_entries && m_entries->contains(p))
        {
            QImage img = QImage::fromData(m_entries->value(p));
            if (!img.isNull()) return img;
        }
        // 2) 相对当前章节目录解析（仅当 p 本身还没有包含当前目录前缀时才拼接，
        //    避免 zip:///OPS/images/a.jpg 解析成 OPS/images/a.jpg 后又被拼成 OPS/OPS/images/a.jpg）
        if (m_entries && !m_curDir.isEmpty() && !p.startsWith('/')
                && !p.startsWith(m_curDir, Qt::CaseInsensitive))
        {
            QString abs = joinPath(m_curDir, p);
            if (m_entries->contains(abs))
            {
                QImage img = QImage::fromData(m_entries->value(abs));
                if (!img.isNull()) return img;
            }
        }
        // 3) FB2 内嵌图片（按 id 查找 base64 解码后的字节）
        if (m_fb2Images)
        {
            QString id = p;
            if (id.startsWith("fb2img://")) id = id.mid(10);
            if (id.startsWith('#'))        id = id.mid(1);
            if (m_fb2Images->contains(id))
            {
                QImage img = QImage::fromData(m_fb2Images->value(id));
                if (!img.isNull()) return img;
            }
        }
    }
    return QTextBrowser::loadResource(type, name);
}

// ============================================================
// MainWindow
// ============================================================
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    // 用自定义的 BookBrowser 替换 .ui 中的占位 QTextBrowser
    m_browser = new BookBrowser(this);
    QLayout *lay = ui->centralWidget->layout();
    if (lay)
    {
        if (ui->textBrowser)
        {
            lay->removeWidget(ui->textBrowser);
            delete ui->textBrowser;
            ui->textBrowser = nullptr;
        }
        lay->addWidget(m_browser);
    }
    else
    {
        setCentralWidget(m_browser);
    }

    m_infoLabel = new QLabel("");
    m_infoLabel->setMinimumWidth(200);
    ui->mainToolBar->addWidget(m_infoLabel);

    m_bookTitle = "电子书";
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::reset()
{
    m_entries.clear();
    m_fb2Images.clear();
    m_chapterHtml.clear();
    m_chapterDirs.clear();
    m_chapterTitles.clear();
    m_curChapter = 0;
    m_zoomSteps  = 0;
    if (m_browser)   m_browser->clear();
    if (m_infoLabel) m_infoLabel->clear();
}

void MainWindow::showBook(char *szBuf, qint64 nFileLen)
{
    reset();
    if (szBuf == nullptr || nFileLen <= 0)
    {
        showMessage("文件为空或无法读取。");
        return;
    }

    // 复制数据由本对象持有：主程序在 sendFileData 返回后会释放原始缓冲区
    m_bookData = QByteArray(szBuf, (int)nFileLen);

    QString fmt = detectFormat(m_bookData);
    bool ok = false;
    if (fmt == "epub")
    {
        ok = loadEpub(m_bookData);
    }
    else if (fmt == "fb2")
    {
        ok = loadFb2(m_bookData);
    }
    else if (fmt == "mobi")
    {
#ifdef WITH_LIBMOBI
        ok = loadMobi(m_bookData);
#else
        showMessage("检测到 MOBI / AZW / AZW3 格式。\n\n"
                     "该格式为私有压缩格式，正文需经 PalmDOC / KF8 解压才能读取，"
                     "需要集成 libmobi 等专用解析库。当前版本暂不支持显示其内容。\n\n"
                     "（EPUB、FB2 格式可正常预览。）");
        return;
#endif
    }
    else
    {
        showMessage("无法识别的电子书格式。\n\n"
                     "当前支持：EPUB、FB2。\n"
                     "MOBI / AZW 等私有格式需专用解析库，暂不支持。");
        return;
    }

    if (!ok)
    {
        showMessage("解析电子书失败，文件可能已损坏或不是受支持的格式。");
        return;
    }
    if (m_chapterHtml.isEmpty())
    {
        showMessage("未在该文件中找到可显示的内容。");
        return;
    }

    loadChapter(0);
}

// ---------- 格式识别（纯靠魔数，插件拿不到文件名）----------
QString MainWindow::detectFormat(const QByteArray &data) const
{
    bool isZip = (data.size() >= 4 && data[0] == 'P' && data[1] == 'K'
                  && data[2] == 3 && data[3] == 4);
    if (isZip)
    {
        ZipReader reader;
        if (reader.open(data))
        {
            bool hasContainer = false, hasFb2 = false;
            for (const QString &n : reader.entryNames())
            {
                if (n.compare("META-INF/container.xml", Qt::CaseInsensitive) == 0
                        || n.endsWith("/META-INF/container.xml", Qt::CaseInsensitive))
                    hasContainer = true;
                if (n.endsWith(".fb2", Qt::CaseInsensitive))
                    hasFb2 = true;
                if (n.compare("mimetype", Qt::CaseInsensitive) == 0)
                {
                    QByteArray mt = reader.entry(n);
                    if (mt.contains("epub")) hasContainer = true;
                }
            }
            if (hasContainer) return "epub";
            if (hasFb2)       return "fb2";
            return "unsupported";
        }
    }

    // 纯文本 FB2（XML）
    if (data.startsWith("<?xml") || data.startsWith("<FictionBook")
            || data.contains("<FictionBook"))
        return "fb2";

    // MOBI / AZW：PalmDOC 头，MOBI 标识在偏移 60
    if (data.size() > 64)
    {
        QByteArray id = data.mid(60, 4);
        if (id == "MOBI" || id == "BOOK" || id == "TEXt")
            return "mobi";
    }
    return "unsupported";
}

// ---------- EPUB 解析 ----------
bool MainWindow::loadEpub(const QByteArray &data)
{
    ZipReader reader;
    if (!reader.open(data)) return false;

    // 展开所有条目到内存映射（键为归一化路径，避免前导 '/' 导致查不到）
    for (const QString &n : reader.entryNames())
    {
        if (n.isEmpty()) continue;
        m_entries[n] = reader.entry(n);
    }

    // 定位 OPF：META-INF/container.xml -> rootfile full-path
    QString containerKey;
    if (m_entries.contains("META-INF/container.xml"))
        containerKey = "META-INF/container.xml";
    else
    {
        for (const QString &k : m_entries.keys())
            if (k.endsWith("/META-INF/container.xml", Qt::CaseInsensitive)) { containerKey = k; break; }
    }
    if (containerKey.isEmpty()) return false;

    QString opfRel = findTagAttr(QString::fromUtf8(m_entries.value(containerKey)), "rootfile", "full-path");
    if (opfRel.isEmpty()) return false;

    QString opfKey = opfRel;
    while (opfKey.startsWith('/')) opfKey.remove(0, 1);
    if (!m_entries.contains(opfKey))
        opfKey = joinPath("", opfRel);
    if (!m_entries.contains(opfKey)) return false;

    QByteArray opf = m_entries.value(opfKey);
    QString opfStr = QString::fromUtf8(opf.constData(), opf.size());
    QString opfDir  = dirOf(opfKey);

    // 书名
    m_bookTitle = textOfTag(opfStr, "title");
    if (m_bookTitle.isEmpty()) m_bookTitle = "未命名电子书";

    // manifest：id -> href
    QMap<QString, QString> id2href;
    for (const QMap<QString, QString> &m : findTags(opfStr, "item"))
    {
        QString id   = m.value("id");
        QString href = m.value("href");
        if (!id.isEmpty() && !href.isEmpty()) id2href[id] = href;
    }

    // spine：有序 idref -> href
    QStringList orderedHrefs;
    for (const QMap<QString, QString> &r : findTags(opfStr, "itemref"))
    {
        QString idref = r.value("idref");
        if (id2href.contains(idref)) orderedHrefs.append(id2href.value(idref));
    }
    // 兜底：spine 为空时按 manifest 中 xhtml 顺序
    if (orderedHrefs.isEmpty())
    {
        for (const QMap<QString, QString> &m : findTags(opfStr, "item"))
        {
            QString mt   = m.value("media-type");
            QString href = m.value("href");
            if (mt.contains("xhtml") || href.endsWith(".xhtml", Qt::CaseInsensitive)
                    || href.endsWith(".html", Qt::CaseInsensitive))
                orderedHrefs.append(href);
        }
    }

    for (const QString &href : orderedHrefs)
    {
        QString absPath = joinPath(opfDir, href);
        while (absPath.startsWith('/')) absPath.remove(0, 1);
        if (!m_entries.contains(absPath)) continue;
        QByteArray html = m_entries.value(absPath);
        QString htmlStr = QString::fromUtf8(html.constData(), html.size());
        m_chapterHtml.append(htmlStr);
        m_chapterDirs.append(dirOf(absPath));
        m_chapterTitles.append(QString("第 %1 章").arg(m_chapterHtml.size()));
    }

    return !m_chapterHtml.isEmpty();
}

// ---------- FB2 解析 ----------
bool MainWindow::loadFb2(const QByteArray &data)
{
    QByteArray xml = data;

    // FB2 也可能以 .fb2.zip 形式存在
    bool isZip = (data.size() >= 4 && data[0] == 'P' && data[1] == 'K'
                  && data[2] == 3 && data[3] == 4);
    if (isZip)
    {
        ZipReader reader;
        if (reader.open(data))
        {
            for (const QString &n : reader.entryNames())
            {
                if (n.isEmpty()) continue;
                m_entries[n] = reader.entry(n);
                if (n.endsWith(".fb2", Qt::CaseInsensitive))
                    xml = reader.entry(n);
            }
        }
    }

    QDomDocument doc;
    if (!doc.setContent(xml)) return false;
    QDomElement root = doc.documentElement();

    // 书名
    QDomNodeList tl = root.elementsByTagName("book-title");
    m_bookTitle = (tl.size() > 0) ? tl.at(0).toElement().text() : QString("未命名电子书");

    // 内嵌二进制图片（base64）
    QDomNodeList bins = root.elementsByTagName("binary");
    for (int i = 0; i < bins.size(); ++i)
    {
        QDomElement e = bins.at(i).toElement();
        QString id = e.attribute("id");
        QByteArray dec = QByteArray::fromBase64(e.text().simplified().toUtf8());
        if (!id.isEmpty() && !dec.isEmpty()) m_fb2Images[id] = dec;
    }

    // 取第一个 <body> 渲染
    QDomNodeList bodies = root.elementsByTagName("body");
    if (bodies.isEmpty()) return false;
    QString html = fb2ToHtml(bodies.at(0));
    if (html.isEmpty()) return false;

    m_chapterHtml.append(html);
    m_chapterDirs.append(QString());
    m_chapterTitles.append(m_bookTitle);
    return true;
}

// ---------- MOBI / AZW / AZW3 解析（依赖 libmobi）----------
bool MainWindow::loadMobi(const QByteArray &data)
{
#ifdef WITH_LIBMOBI
    if (data.isEmpty()) return false;

    // libmobi 没有「内存加载」接口，先把字节落一个临时文件再加载
    QString tmpPath = QDir::temp().absoluteFilePath(
        QString("fw_mobi_%1.mobi").arg(quintptr(this), 0, 16));
    {
        QFile tf(tmpPath);
        if (!tf.open(QIODevice::WriteOnly) || tf.write(data) != data.size())
        {
            tf.remove();
            return false;
        }
        tf.close();
    }

    MOBIData *m = mobi_init();
    if (!m) { QFile::remove(tmpPath); return false; }

    MOBI_RET ret = mobi_load_filename(m, tmpPath.toLocal8Bit().constData());
    QFile::remove(tmpPath);
    if (ret != MOBI_SUCCESS) { mobi_free(m); return false; }

    // 书名
    char *title = mobi_meta_get_title(m);
    if (title && *title)
        m_bookTitle = QString::fromUtf8(title);
    else
    {
        char fn[256];
        if (mobi_get_fullname(m, fn, sizeof(fn)) == MOBI_SUCCESS && fn[0])
            m_bookTitle = QString::fromUtf8(fn);
    }
    if (title) ::free(title);
    if (m_bookTitle.isEmpty()) m_bookTitle = "未命名电子书";

    MOBIRawml *rawml = mobi_init_rawml(m);
    if (!rawml) { mobi_free(m); return false; }
    ret = mobi_parse_rawml(rawml, m);
    if (ret != MOBI_SUCCESS) { mobi_free_rawml(rawml); mobi_free(m); return false; }

    // 拼接所有 flow 片段为可显示的 HTML（KF8 会把正文拆成多个 part）
    QString html;
    for (MOBIPart *p = rawml->flow; p; p = p->next)
    {
        if (p->data && p->size > 0)
            html += QString::fromUtf8(reinterpret_cast<const char *>(p->data),
                                      static_cast<int>(p->size));
    }

    mobi_free_rawml(rawml);
    mobi_free(m);

    if (html.isEmpty()) return false;

    m_chapterHtml.append(html);
    m_chapterDirs.append(QString());
    m_chapterTitles.append(m_bookTitle);
    return true;
#else
    Q_UNUSED(data);
    return false;
#endif
}

QString MainWindow::fb2ToHtml(const QDomNode &node)
{
    QString out;
    QDomNode child = node.firstChild();
    while (!child.isNull())
    {
        if (child.isElement())
        {
            QDomElement e   = child.toElement();
            QString     tag = e.tagName().toLower();
            if (tag == "title")         out += "<h2>" + escapeText(e.text()) + "</h2>";
            else if (tag == "subtitle") out += "<h3>" + escapeText(e.text()) + "</h3>";
            else if (tag == "p")        out += "<p>" + escapeText(e.text()) + "</p>";
            else if (tag == "cite")     out += "<blockquote>" + escapeText(e.text()) + "</blockquote>";
            else if (tag == "strong" || tag == "bold")    out += "<b>" + escapeText(e.text()) + "</b>";
            else if (tag == "emphasis" || tag == "italic") out += "<i>" + escapeText(e.text()) + "</i>";
            else if (tag == "code")     out += "<pre>" + escapeText(e.text()) + "</pre>";
            else if (tag == "empty-line") out += "<br>";
            else if (tag == "image")
            {
                QString href = e.attribute("l:href", e.attribute("href"));
                if (href.startsWith('#')) href = href.mid(1);
                if (!href.isEmpty()) out += "<img src=\"fb2img://" + href + "\">";
            }
            else out += fb2ToHtml(e); // section 等容器，递归
        }
        else if (child.isText())
        {
            out += escapeText(child.toText().data());
        }
        child = child.nextSibling();
    }
    return out;
}

// ---------- 章节加载 ----------
void MainWindow::loadChapter(int idx)
{
    if (idx < 0 || idx >= m_chapterHtml.size()) return;
    m_curChapter = idx;

    m_browser->setEntries(&m_entries);
    m_browser->setFb2Images(&m_fb2Images);
    m_browser->setCurDir(m_chapterDirs.at(idx));

    QString base = m_chapterDirs.at(idx);
    if (!base.isEmpty() && !base.endsWith('/')) base += '/';
    // 用三斜杠 zip:/// 使路径成为 URL 的 path 而非 host；Qt 会对 host 做大小写归一化，
    // 例如 zip://OPS/ 会变成 zip://ops/，导致无法匹配条目名为大写的 OPS/images/... 。
    m_browser->document()->setBaseUrl(QUrl("zip:///" + base));

    m_browser->setHtml(m_chapterHtml.at(idx));

    // 重新应用累计缩放
    if (m_zoomSteps != 0) m_browser->zoomIn(m_zoomSteps);

    m_infoLabel->setText(QString("%1  |  %2 / %3")
                         .arg(m_bookTitle)
                         .arg(idx + 1)
                         .arg(m_chapterHtml.size()));
    ui->actionPrevChapter->setEnabled(idx > 0);
    ui->actionNextChapter->setEnabled(idx < m_chapterHtml.size() - 1);
}

void MainWindow::showMessage(const QString &msg)
{
    QString html = msg;
    html.replace("\n", "<br>");
    m_browser->setHtml("<div style='padding:28px;font-family:sans-serif;line-height:1.7;color:#555;'>"
                       + html + "</div>");
    m_infoLabel->setText("提示");
    ui->actionPrevChapter->setEnabled(false);
    ui->actionNextChapter->setEnabled(false);
}

// ---------- 缩放 / 翻章 ----------
void MainWindow::on_actionZoomIn_triggered()  { m_zoomSteps++; m_browser->zoomIn(1); }
void MainWindow::on_actionZoomOut_triggered() { m_zoomSteps--; m_browser->zoomOut(1); }
void MainWindow::on_actionPrevChapter_triggered()
{
    if (m_curChapter > 0) loadChapter(m_curChapter - 1);
}
void MainWindow::on_actionNextChapter_triggered()
{
    if (m_curChapter < m_chapterHtml.size() - 1) loadChapter(m_curChapter + 1);
}

// ---------- 轻量 XML 辅助 ----------
QList<QMap<QString, QString>> MainWindow::findTags(const QString &xml, const QString &tag) const
{
    QList<QMap<QString, QString>> res;
    // (?:<|:) 允许命名空间前缀；(?=[>\s/]) 防止把 title-info 误判为 title
    QString pat = "(?:<|:)" + QRegularExpression::escape(tag) + "(?=[>\\s/])([^>]*)";
    QRegularExpression re(pat, QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator it = re.globalMatch(xml);
    while (it.hasNext())
    {
        QRegularExpressionMatch m = it.next();
        QString attrs = m.captured(1);
        QMap<QString, QString> amap;
        QRegularExpression are("([\\w:-]+)\\s*=\\s*\"([^\"]*)\"");
        QRegularExpressionMatchIterator ait = are.globalMatch(attrs);
        while (ait.hasNext())
        {
            QRegularExpressionMatch am = ait.next();
            amap[am.captured(1)] = am.captured(2);
        }
        res.append(amap);
    }
    return res;
}

QString MainWindow::findTagAttr(const QString &xml, const QString &tag, const QString &name) const
{
    QList<QMap<QString, QString>> tags = findTags(xml, tag);
    for (const QMap<QString, QString> &m : tags)
        if (m.contains(name)) return m.value(name);
    return QString();
}

QString MainWindow::textOfTag(const QString &xml, const QString &tag) const
{
    QString pat = "(?:<|:)" + QRegularExpression::escape(tag) + "(?=[>\\s/])[^>]*>(.*?)(?:<|:)/"
                  + QRegularExpression::escape(tag) + "(?=[>\\s/])";
    QRegularExpression re(pat, QRegularExpression::CaseInsensitiveOption
                               | QRegularExpression::DotMatchesEverythingOption);
    QRegularExpressionMatch m = re.match(xml);
    if (m.hasMatch()) return m.captured(1).trimmed();
    return QString();
}

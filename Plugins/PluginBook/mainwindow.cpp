#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "zipreader.h"
#include <QLayout>
#include <QDomDocument>
#include <QRegularExpression>
#include <limits>
#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <QUrl>
#include <QLabel>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
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
    QStringList parts    = base.split('/', Qt::SkipEmptyParts);
    QStringList relParts = rel.split('/', Qt::SkipEmptyParts);
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
// 资源上限 / 截断状态（文件作用域）
// ============================================================
// ZIP 展开上限，EPUB 与 FB2 两个分支共用：
// - 4096 个条目：正常电子书（含图片）通常远小于此，封顶防止中央目录被灌入海量条目；
// - 解压后 256 MiB：与 showBook 的整书大小预算一致，且单条目另有 ZipReader 的 64 MiB 上限。
// 超限时只保留已展开的部分并标记截断，不再继续解压，避免 ZIP bomb 打爆内存。
static bool expandZipEntries(ZipReader &reader, QMap<QString, QByteArray> &entries)
{
    constexpr int    kMaxEntries    = 4096;
    constexpr qint64 kMaxTotalBytes = 256LL * 1024 * 1024;   // 256 MiB
    qint64 totalBytes = 0;
    int    entryCount = 0;

    for (const QString &n : reader.entryNames())
    {
        if (n.isEmpty()) continue;
        // 按遍历到的条目计数（而非 map 中唯一键数量），同名的重复条目也会受限。
        if (entryCount >= kMaxEntries || totalBytes >= kMaxTotalBytes)
        {
            qWarning() << "[PluginBook] ZIP 条目过多或解压后体积超限，停止展开";
            return false;
        }
        ++entryCount;

        const QByteArray entryData = reader.entry(n);
        if (entryData.isEmpty()) continue;
        const qint64 entryBytes = static_cast<qint64>(entryData.size());
        if (entryBytes > kMaxTotalBytes - totalBytes)
        {
            qWarning() << "[PluginBook] ZIP 条目累计体积超限，停止展开";
            return false;
        }
        totalBytes += entryBytes;
        entries[n] = entryData;
    }
    return true;
}

// 截断状态放在 QObject 动态属性里，避免为本插件新增头文件成员；
// 读取完成后由 showBook()/loadChapter() 统一显示提示。
static const char kTruncatedProperty[]        = "fwBookTruncated";
static const char kTruncatedReasonsProperty[] = "fwBookTruncatedReasons";

static void resetTruncation(QObject *owner)
{
    owner->setProperty(kTruncatedProperty, false);
    owner->setProperty(kTruncatedReasonsProperty, QStringList());
}

static void markTruncated(QObject *owner, const QString &reason)
{
    owner->setProperty(kTruncatedProperty, true);
    QStringList reasons = owner->property(kTruncatedReasonsProperty).toStringList();
    if (!reason.isEmpty() && !reasons.contains(reason))
        reasons.append(reason);
    owner->setProperty(kTruncatedReasonsProperty, reasons);
}

static QString truncationTip(const QObject *owner)
{
    if (!owner->property(kTruncatedProperty).toBool()) return QString();
    QString tip = QStringLiteral("部分内容超出处理上限，已截断显示。");
    const QStringList reasons = owner->property(kTruncatedReasonsProperty).toStringList();
    if (!reasons.isEmpty())
        tip += QStringLiteral("（%1）").arg(reasons.join(QStringLiteral("；")));
    return tip;
}

// 图片预算：
// - 单张图片的编码/压缩字节上限 32 MiB：正常插图远小于此，同时避免把超大图片交给解码器；
// - 整本书累计解码预算 256 MiB：按 width*height*4 保守估算 QImage 的 ARGB32 占用，
//   多张接近单图上限的图片累加也不会超过该预算；超限图片直接跳过，不进入文档资源缓存。
static constexpr qint64 kMaxImageCompressedBytes = 32LL * 1024 * 1024;    // 32 MiB
static constexpr qint64 kMaxDecodedImageBytes    = 256LL * 1024 * 1024;   // 256 MiB
static const char kDecodedImageBytesProperty[]   = "fwBookDecodedImageBytes";

static QImage decodeImageWithBudget(const QByteArray &data, qint64 &decodedBytes, bool &limitHit)
{
    if (data.isEmpty()) return QImage();
    if (data.size() > kMaxImageCompressedBytes)
    {
        limitHit = true;
        qWarning() << "[PluginBook] 单张图片超过 32 MiB，跳过";
        return QImage();
    }

    // 先用 QImageReader 读取头部尺寸做预算预检，避免为一张必定超预算的图片
    // 真正分配数百 MiB 内存；部分格式拿不到尺寸时再回退到解码后检查。
    QBuffer buffer;
    buffer.setData(data);
    if (!buffer.open(QIODevice::ReadOnly)) return QImage();
    QImageReader reader(&buffer);
    const QSize declaredSize = reader.size();
    if (declaredSize.isValid())
    {
        // 先算像素数再与预算/4 比较，避免 width*height*4 在恶意超宽高下溢出 qint64。
        const qint64 pixels = qint64(declaredSize.width()) * qint64(declaredSize.height());
        if (pixels > (kMaxDecodedImageBytes - decodedBytes) / 4)
        {
            limitHit = true;
            qWarning() << "[PluginBook] 图片解码后尺寸超过累计预算，跳过";
            return QImage();
        }
    }
    QImage img = reader.read();
    if (img.isNull()) return QImage();

    // 用像素估算解码后的内存占用（width*height*4），比按文件大小估算更贴近实际分配。
    const qint64 pixels = qint64(img.width()) * qint64(img.height());
    if (pixels > (kMaxDecodedImageBytes - decodedBytes) / 4)
    {
        limitHit = true;
        qWarning() << "[PluginBook] 图片累计解码字节超过 256 MiB，跳过";
        return QImage();
    }
    decodedBytes += pixels * 4;
    return img;
}

// 轻量 XML 标签扫描（带数量上限）：
// OPF 里的 item/itemref 数量完全由文件内容决定，若先 findTags() 全量物化再截断，
// 一个 64 MiB 的 OPF 仍可塞入数百万个标签把 QList<QMap> 撑爆。这里在正则迭代阶段
// 就按 maxTags 停住，并通过 limitReached 告知调用方已截断。
static QList<QMap<QString, QString>> findTagsLimited(const QString &xml, const QString &tag,
                                                     int maxTags, bool *limitReached)
{
    QList<QMap<QString, QString>> res;
    if (limitReached) *limitReached = false;

    // (?:<|:) 允许命名空间前缀；(?=[>\s/]) 防止把 title-info 误判为 title
    QString pat = "(?:<|:)" + QRegularExpression::escape(tag) + "(?=[>\\s/])([^>]*)";
    QRegularExpression re(pat, QRegularExpression::CaseInsensitiveOption);
    QRegularExpression are("([\\w:-]+)\\s*=\\s*\"([^\"]*)\"");
    QRegularExpressionMatchIterator it = re.globalMatch(xml);
    while (it.hasNext())
    {
        if (res.size() >= maxTags)
        {
            if (limitReached) *limitReached = true;
            break;
        }

        QRegularExpressionMatch m = it.next();
        QString attrs = m.captured(1);
        QMap<QString, QString> amap;
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

        // 累计预算挂在 BookBrowser 实例上，reset() 打开新书时归零；
        // 同一本书内所有章节的图片共用该预算。
        qint64 decodedBytes = property(kDecodedImageBytesProperty).toLongLong();
        if (decodedBytes < 0) decodedBytes = 0;
        if (decodedBytes > kMaxDecodedImageBytes) decodedBytes = kMaxDecodedImageBytes;
        bool limitHit = false;
        auto decode = [&](const QByteArray &raw) -> QVariant
        {
            QImage img = decodeImageWithBudget(raw, decodedBytes, limitHit);
            if (img.isNull()) return QVariant();
            setProperty(kDecodedImageBytesProperty, QVariant::fromValue<qint64>(decodedBytes));
            return img;
        };
        // 1) 直接按 zip 内路径查找
        if (m_entries && m_entries->contains(p))
        {
            QVariant img = decode(m_entries->value(p));
            if (img.isValid()) return img;
            // 明确因预算被拒时直接返回，避免再换路径重复解码同一张大图。
            if (limitHit) return QVariant();
        }
        // 2) 相对当前章节目录解析（仅当 p 本身还没有包含当前目录前缀时才拼接，
        //    避免 zip:///OPS/images/a.jpg 解析成 OPS/images/a.jpg 后又被拼成 OPS/OPS/images/a.jpg）
        if (m_entries && !m_curDir.isEmpty() && !p.startsWith('/')
                && !p.startsWith(m_curDir, Qt::CaseInsensitive))
        {
            QString abs = joinPath(m_curDir, p);
            if (m_entries->contains(abs))
            {
                QVariant img = decode(m_entries->value(abs));
                if (img.isValid()) return img;
                if (limitHit) return QVariant();
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
                QVariant img = decode(m_fb2Images->value(id));
                if (img.isValid()) return img;
                if (limitHit) return QVariant();
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
    resetTruncation(this);
    m_entries.clear();
    m_fb2Images.clear();
    m_chapterHtml.clear();
    m_chapterDirs.clear();
    m_chapterTitles.clear();
    m_curChapter = 0;
    m_zoomSteps  = 0;
    if (m_browser)
    {
        // 新书重新开始累计图片解码预算；旧 QTextDocument 的资源缓存随 clear() 释放。
        m_browser->setProperty(kDecodedImageBytesProperty, QVariant::fromValue<qint64>(0));
        m_browser->clear();
    }
    if (m_infoLabel) m_infoLabel->clear();
}

void MainWindow::clearContent()
{
    reset();
    m_bookData.clear();
    m_bookTitle = QStringLiteral("电子书");
    m_sourceName.clear();
    showMessage(QStringLiteral("文件为空或无法读取。"));
}

void MainWindow::showBook(char *szBuf, qint64 nFileLen)
{
    reset();
    if (szBuf == nullptr || nFileLen <= 0)
    {
        showMessage("文件为空或无法读取。");
        return;
    }

    // 复制数据由本对象持有：主程序在 sendFileData 返回后会释放原始缓冲区。
    // Qt6 的 QByteArray 长度类型为 qsizetype（64 位），无需强转 int。
    // 64 位下 qsizetype 与 qint64 同宽，原「超过 qsizetype::max()」判断恒为
    // false（死代码）；这里与 loadEpub 的解压后总量上限保持一致。
    constexpr qint64 kMaxBookBytes = 256 * 1024 * 1024;   // 256 MiB
    if (nFileLen > kMaxBookBytes)
    {
        showMessage("电子书文件过大，无法载入内存。");
        return;
    }
    m_bookData = QByteArray(szBuf, static_cast<qsizetype>(nFileLen));

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
        QString msg = QStringLiteral("解析电子书失败，文件可能已损坏或不是受支持的格式。");
        const QString tip = truncationTip(this);
        if (!tip.isEmpty()) msg += QStringLiteral("\n\n") + tip;
        showMessage(msg);
        return;
    }
    if (m_chapterHtml.isEmpty())
    {
        QString msg = QStringLiteral("未在该文件中找到可显示的内容。");
        const QString tip = truncationTip(this);
        if (!tip.isEmpty()) msg += QStringLiteral("\n\n") + tip;
        showMessage(msg);
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

    // 展开条目到内存映射（键为归一化路径，避免前导 '/' 导致查不到）。
    // ZIP bomb 防护：条目数/解压总量上限由 expandZipEntries() 统一提供，
    // 与 FB2 的 .fb2.zip 分支共用；超限时保留已展开部分并标记截断。
    if (!expandZipEntries(reader, m_entries))
        markTruncated(this, QStringLiteral("EPUB ZIP 条目超限"));

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
    if (m_bookTitle.isEmpty()) m_bookTitle = fallbackTitle();

    // manifest 与 spine 的标签数量都由 OPF 内容控制：
    // - 4096 与 ZIP 条目上限对齐，正常电子书的 manifest 远小于此；
    // - 解析时就传入上限，避免 findTags() 先物化数百万个 QMap 再截断。
    constexpr int    kMaxChapters     = 4096;
    constexpr qint64 kMaxChapterBytes = 256LL * 1024 * 1024;   // 256 MiB

    // manifest：id -> href
    QMap<QString, QString> id2href;
    bool manifestLimitReached = false;
    for (const QMap<QString, QString> &m
         : findTagsLimited(opfStr, "item", kMaxChapters, &manifestLimitReached))
    {
        QString id   = m.value("id");
        QString href = m.value("href");
        if (!id.isEmpty() && !href.isEmpty()) id2href[id] = href;
    }
    if (manifestLimitReached)
    {
        qWarning() << "[PluginBook] EPUB manifest 条目数超限，停止解析";
        markTruncated(this, QStringLiteral("EPUB manifest 条目超限"));
    }

    // spine：有序 idref -> href。
    // 章节上限 4096：itemref 可以反复引用同一个小 xhtml，若不封顶会生成
    // 同样长度的 chapterHtml/chapterDirs/chapterTitles 三个列表，绕过 ZIP 条目上限。
    // 正文字节上限 256 MiB：与整书/解压后总量预算一致，防止重复引用累计出超大正文。
    QStringList orderedHrefs;
    bool itemrefLimitReached = false;
    for (const QMap<QString, QString> &r
         : findTagsLimited(opfStr, "itemref", kMaxChapters, &itemrefLimitReached))
    {
        if (orderedHrefs.size() >= kMaxChapters)
        {
            qWarning() << "[PluginBook] EPUB spine 章节数超限，停止展开";
            markTruncated(this, QStringLiteral("EPUB 章节数超限"));
            break;
        }
        QString idref = r.value("idref");
        if (id2href.contains(idref)) orderedHrefs.append(id2href.value(idref));
    }
    if (itemrefLimitReached)
    {
        qWarning() << "[PluginBook] EPUB spine 标签数超限，停止解析";
        markTruncated(this, QStringLiteral("EPUB 章节数超限"));
    }
    // 兜底：spine 为空时按 manifest 中 xhtml 顺序（同样限制章节数）
    if (orderedHrefs.isEmpty())
    {
        bool fallbackLimitReached = false;
        for (const QMap<QString, QString> &m
             : findTagsLimited(opfStr, "item", kMaxChapters, &fallbackLimitReached))
        {
            if (orderedHrefs.size() >= kMaxChapters)
            {
                qWarning() << "[PluginBook] EPUB manifest 章节数超限，停止展开";
                markTruncated(this, QStringLiteral("EPUB 章节数超限"));
                break;
            }
            QString mt   = m.value("media-type");
            QString href = m.value("href");
            if (mt.contains("xhtml") || href.endsWith(".xhtml", Qt::CaseInsensitive)
                    || href.endsWith(".html", Qt::CaseInsensitive))
                orderedHrefs.append(href);
        }
        if (fallbackLimitReached)
        {
            qWarning() << "[PluginBook] EPUB manifest 标签数超限，停止解析";
            markTruncated(this, QStringLiteral("EPUB manifest 条目超限"));
        }
    }

    qint64 totalChapterBytes = 0;
    for (const QString &href : orderedHrefs)
    {
        if (m_chapterHtml.size() >= kMaxChapters)
        {
            markTruncated(this, QStringLiteral("EPUB 章节数超限"));
            break;
        }
        QString absPath = joinPath(opfDir, href);
        while (absPath.startsWith('/')) absPath.remove(0, 1);
        if (!m_entries.contains(absPath)) continue;
        QByteArray html = m_entries.value(absPath);
        const qint64 htmlBytes = static_cast<qint64>(html.size());
        if (htmlBytes > kMaxChapterBytes - totalChapterBytes)
        {
            qWarning() << "[PluginBook] EPUB 正文字节数超限，停止展开";
            markTruncated(this, QStringLiteral("EPUB 正文字节超限"));
            break;
        }
        totalChapterBytes += htmlBytes;
        QString htmlStr = QString::fromUtf8(html.constData(), html.size());
        // 三个列表始终同步追加，保证章节序号、标题和章节目录一一对应。
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
            // 与 EPUB 分支共用条目数/解压总量上限，避免 .fb2.zip 绕过 ZIP bomb 防护。
            if (!expandZipEntries(reader, m_entries))
                markTruncated(this, QStringLiteral("FB2 ZIP 条目超限"));
            for (const QString &n : reader.entryNames())
            {
                if (n.endsWith(".fb2", Qt::CaseInsensitive))
                    xml = m_entries.value(n);
            }
        }
    }

    QDomDocument doc;
    if (!doc.setContent(xml)) return false;
    QDomElement root = doc.documentElement();

    // 书名
    QDomNodeList tl = root.elementsByTagName("book-title");
    m_bookTitle = (tl.size() > 0) ? tl.at(0).toElement().text() : QString();
    if (m_bookTitle.trimmed().isEmpty()) m_bookTitle = fallbackTitle();

    // 内嵌二进制图片（base64）
    QDomNodeList bins = root.elementsByTagName("binary");
    for (int i = 0; i < bins.size(); ++i)
    {
        QDomElement e = bins.at(i).toElement();
        QString id = e.attribute("id");
        QByteArray dec = QByteArray::fromBase64(e.text().simplified().toUtf8());
        // 单张图片 32 MiB 上限同样覆盖 FB2 的 base64 解码结果，避免在进入
        // BookBrowser 之前就把超大图片放进 m_fb2Images、绕过 loadResource 的检查。
        if (dec.size() > kMaxImageCompressedBytes)
        {
            qWarning() << "[PluginBook] FB2 内嵌图片超过 32 MiB，跳过";
            continue;
        }
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
    if (m_bookTitle.isEmpty()) m_bookTitle = fallbackTitle();

    MOBIRawml *rawml = mobi_init_rawml(m);
    if (!rawml) { mobi_free(m); return false; }
    ret = mobi_parse_rawml(rawml, m);
    if (ret != MOBI_SUCCESS) { mobi_free_rawml(rawml); mobi_free(m); return false; }

    // 拼接所有 flow 片段为可显示的 HTML（KF8 会把正文拆成多个 part）。
    // 上限 256 MiB：与其它格式的正文/解压预算一致；拼接后的 QString 还要再占一份内存，
    // 因此必须在累加阶段封顶，不能等拼完再检查。
    constexpr qint64 kMaxMobiBytes = 256LL * 1024 * 1024;   // 256 MiB
    qint64 totalBytes = 0;
    QString html;
    for (MOBIPart *p = rawml->flow; p; p = p->next)
    {
        if (!p->data || p->size == 0) continue;
        // p->size 是 size_t，直接 static_cast<int> 会在超过 INT_MAX 时截断成负数，
        // 进而给 QString::fromUtf8 传入负长度；这里先做防御性检查。
        if (p->size > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            qWarning() << "[PluginBook] MOBI flow 片段超过 INT_MAX，停止拼接";
            markTruncated(this, QStringLiteral("MOBI 正文片段过大"));
            break;
        }
        const qint64 partBytes = static_cast<qint64>(p->size);
        if (partBytes > kMaxMobiBytes - totalBytes)
        {
            qWarning() << "[PluginBook] MOBI 正文累计超过 256 MiB，停止拼接";
            markTruncated(this, QStringLiteral("MOBI 正文超过 256 MiB"));
            break;
        }
        totalBytes += partBytes;
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
                // href 来自 XML 属性：QDom 会把 &quot; 解码成双引号，直接拼入
                // src="..." 可闭合属性并注入标签，因此必须做 HTML 属性转义。
                if (!href.isEmpty())
                    out += "<img src=\"fb2img://" + href.toHtmlEscaped() + "\">";
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

    QString info = QString("%1  |  %2 / %3")
                   .arg(m_bookTitle)
                   .arg(idx + 1)
                   .arg(m_chapterHtml.size());
    // 截断状态在状态栏持续可见，避免只在解析结束时提示一次。
    if (property(kTruncatedProperty).toBool())
        info += QStringLiteral("  |  已截断");
    m_infoLabel->setText(info);
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
    // 通用辅助函数保持全量语义；需要防 DoS 的 OPF 解析路径改用 findTagsLimited()。
    return findTagsLimited(xml, tag, std::numeric_limits<int>::max(), nullptr);
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
    const QString t = QRegularExpression::escape(tag);
    // 支持带命名空间前缀的标签：<dc:title>…</dc:title>（EPUB OPF 的常见写法）
    const QString ns = QStringLiteral("(?:[A-Za-z_][\\w.-]*:)?");
    QString pat = QStringLiteral("<") + ns + t
                + QStringLiteral("(?=[>\\s/])[^>]*>(.*?)</") + ns + t
                + QStringLiteral("(?=[>\\s/])");
    QRegularExpression re(pat, QRegularExpression::CaseInsensitiveOption
                               | QRegularExpression::DotMatchesEverythingOption);
    QRegularExpressionMatch m = re.match(xml);
    if (m.hasMatch()) return m.captured(1).trimmed();

    // 兼容旧式 / 非标准写法（:tag … :/tag）
    QString oldPat = QStringLiteral("(?:<|:)") + t
                   + QStringLiteral("(?=[>\\s/])[^>]*>(.*?)(?:<|:)/") + t
                   + QStringLiteral("(?=[>\\s/])");
    QRegularExpression oldRe(oldPat, QRegularExpression::CaseInsensitiveOption
                                     | QRegularExpression::DotMatchesEverythingOption);
    m = oldRe.match(xml);
    if (m.hasMatch()) return m.captured(1).trimmed();
    return QString();
}

QString MainWindow::fallbackTitle() const
{
    if (!m_sourceName.isEmpty())
        return QFileInfo(m_sourceName).completeBaseName();
    return QStringLiteral("未命名电子书");
}

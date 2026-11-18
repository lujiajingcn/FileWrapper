#include "slidesdocument.h"

#include <QDomDocument>
#include <QDomElement>
#include <QHash>
#include <QStringList>
#include <algorithm>
#include <functional>
#include "xmlguard.h"

namespace {

constexpr int    kMaxSlides   = 200;
constexpr int    kMaxBlocks   = 2000;
constexpr double kEmuPerPt    = 12700.0;      // 1pt = 12700 EMU
constexpr qint64 kMaxImageBytes = 32LL * 1024 * 1024;
constexpr qint64 kMaxDecodedImageBytes = 256LL * 1024 * 1024;   // 整份文档累计解码预算

QString esc(const QString &s)
{
    QString r;
    r.reserve(s.size() + 16);
    for (const QChar ch : s)
    {
        switch (ch.unicode())
        {
        case '&': r += QLatin1String("&amp;"); break;
        case '<': r += QLatin1String("&lt;");  break;
        case '>': r += QLatin1String("&gt;");  break;
        default:  r += ch;                     break;
        }
    }
    return r;
}

QString fmtNum(double v)
{
    QString s = QString::number(v, 'f', 2);
    while (s.endsWith(QLatin1Char('0'))) s.chop(1);
    if (s.endsWith(QLatin1Char('.'))) s.chop(1);
    return s;
}

// 相对路径归一化："ppt/slides/" + "../media/image1.png" -> "ppt/media/image1.png"
QString joinPath(const QString &baseDir, const QString &rel)
{
    const QString full = baseDir + QLatin1Char('/') + rel;
    QStringList parts = full.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QStringList out;
    for (const QString &p : parts)
    {
        if (p == QLatin1String(".")) continue;
        if (p == QLatin1String("..")) { if (!out.isEmpty()) out.removeLast(); continue; }
        out.append(p);
    }
    return out.join(QLatin1Char('/'));
}

// rels 只解析一次：Relationship Id -> Target。
// 原实现每个 sldId / 每张图片都重新 setContent 解析整份 rels，畸形文档可借此拖死 UI。
QHash<QString, QString> parseRels(const QByteArray &relsXml)
{
    QHash<QString, QString> targets;
    QDomDocument doc;
    if (!doc.setContent(relsXml)) return targets;
    for (QDomElement e = doc.documentElement().firstChildElement(QStringLiteral("Relationship"));
         !e.isNull(); e = e.nextSiblingElement(QStringLiteral("Relationship")))
    {
        targets.insert(e.attribute(QStringLiteral("Id")), e.attribute(QStringLiteral("Target")));
    }
    return targets;
}

// 整份文档累计解码字节（QImage 像素按 4 字节估算）。累计值由解析函数内的局部变量保存：
// addImage 没有额外参数，一份文档也只会走 pptx 或 odp 其中一条路径，作用域恰好就是整份文档。
bool chargeDecodeBudget(qint64 *used, const QImage &img)
{
    if (used == nullptr || img.isNull()) return false;
    const qint64 need = qint64(img.width()) * img.height() * 4;
    if (*used + need > kMaxDecodedImageBytes) return false;
    *used += need;
    return true;
}

QString attrOf(const QDomElement &el, const QString &name)
{
    return el.isNull() ? QString() : el.attribute(name);
}

} // namespace

void SlidesDocument::reset()
{
    m_zip = ZipReader();
    m_slides.clear();
    m_images.clear();
    m_kind.clear();
    m_imageSeq = 0;
    m_truncated = false;
    m_ok = false;
}

QString SlidesDocument::normalizePath(const QString &baseDir, const QString &rel) const
{
    return joinPath(baseDir, rel);
}

void SlidesDocument::addImage(const QByteArray &bytes, double wPt, double hPt, Block *block)
{
    if (bytes.isEmpty() || bytes.size() > kMaxImageBytes || block == nullptr) return;

    const QImage img = QImage::fromData(bytes);
    if (img.isNull()) return;      // WMF/EMF 等 Qt 不能直接解码的格式

    const QString url = m_urlPrefix + QString::number(++m_imageSeq);
    m_images.insert(url, img);
    block->imageUrl = url;
    // 文档属性里的显示尺寸不可信：必须有上限，否则 2e8pt 会把页面撑成超宽布局。
    const double w = qBound(1.0, wPt > 0.0 ? wPt : 240.0, 10000.0);
    const double h = qBound(1.0, hPt > 0.0 ? hPt
                                           : (w * img.height() / qMax(1, img.width())),
                            10000.0);
    block->imgW = w;
    block->imgH = h;
}

bool SlidesDocument::open(const QByteArray &data, QString *error)
{
    reset();
    auto fail = [&](const QString &msg) {
        m_ok = false;
        if (error) *error = msg;
        return false;
    };

    if (data.isEmpty())
        return fail(QStringLiteral("文件为空（0 字节），没有可显示的内容。"));
    if (!data.startsWith(QByteArrayLiteral("PK\x03\x04")))
        return fail(QStringLiteral("无法解析：这不是 pptx/odp（ZIP 容器读取失败）。"));
    if (!m_zip.open(data))
        return fail(QStringLiteral("无法解析：ZIP 容器读取失败（文件可能已损坏）。"));

    if (m_zip.entry(QStringLiteral("mimetype")).contains("opendocument.presentation"))
        return parseOdp(error);

    if (m_zip.contains(QStringLiteral("ppt/presentation.xml")))
        return parsePptx(error);

    return fail(QStringLiteral("无法识别该演示文件：既不是 .pptx（ppt/presentation.xml）"
                               "也不是 .odp（opendocument.presentation）。"));
}

bool SlidesDocument::parsePptx(QString *error)
{
    m_kind = QStringLiteral("pptx");
    auto fail = [&](const QString &msg) {
        m_ok = false;
        if (error) *error = msg;
        return false;
    };

    const QByteArray presXml = m_zip.entry(QStringLiteral("ppt/presentation.xml"));
    if (!xmlWithinTagBudget(presXml))
        return fail(QStringLiteral("pptx XML 元素过多。"));
    QDomDocument pres;
    if (!pres.setContent(presXml))
        return fail(QStringLiteral("无法解析 ppt/presentation.xml。"));

    const QByteArray presRels = m_zip.entry(QStringLiteral("ppt/_rels/presentation.xml.rels"));
    const QHash<QString, QString> presRelTargets = parseRels(presRels);

    // 页序：p:sldIdLst/p:sldId r:id
    QStringList slidePaths;
    QDomElement sldIdLst;
    std::function<void(const QDomElement &)> findList = [&](const QDomElement &el) {
        if (!sldIdLst.isNull()) return;
        if (el.tagName() == QLatin1String("p:sldIdLst")) { sldIdLst = el; return; }
        for (QDomElement c = el.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
            findList(c);
    };
    findList(pres.documentElement());

    // 先按 kMaxSlides 截断再查表：畸形文档的 sldId 数量不受限，逐个处理会拖死 UI
    int sldIdCount = 0;
    for (QDomElement e = sldIdLst.firstChildElement(QStringLiteral("p:sldId")); !e.isNull();
         e = e.nextSiblingElement(QStringLiteral("p:sldId")))
    {
        if (++sldIdCount > kMaxSlides) { m_truncated = true; break; }
        const QString target = presRelTargets.value(e.attribute(QStringLiteral("r:id")));
        if (target.isEmpty()) continue;
        QString path = normalizePath(QStringLiteral("ppt"), target);
        if (!path.startsWith(QLatin1String("ppt/"))) path = QStringLiteral("ppt/") + path;
        slidePaths.append(path);
    }
    if (slidePaths.isEmpty())
        return fail(QStringLiteral("演示文稿里没有任何幻灯片。"));

    // 整份文档的累计解码预算（像素估算）：超预算的图片不再加入文档
    qint64 decodedBytes = 0;

    for (int si = 0; si < slidePaths.size() && si < kMaxSlides; ++si)
    {
        const QString path = slidePaths.at(si);
        QDomDocument slideDoc;
        const QByteArray slideXml = m_zip.entry(path);
        if (!xmlWithinTagBudget(slideXml) || !slideDoc.setContent(slideXml)) continue;

        Slide slide;
        const QString slideDir = path.left(path.lastIndexOf(QLatin1Char('/')));
        const QByteArray slideRels = m_zip.entry(slideDir + QStringLiteral("/_rels/")
                    + path.mid(path.lastIndexOf(QLatin1Char('/')) + 1) + QStringLiteral(".rels"));
        const QHash<QString, QString> slideRelTargets = parseRels(slideRels);

        // 收集所有 p:sp / p:pic（含图片）
        std::function<void(const QDomElement &)> walk = [&](const QDomElement &el) {
            if (slide.blocks.size() >= kMaxBlocks) { m_truncated = true; return; }

            const QString tag = el.tagName();
            if (tag == QLatin1String("p:sp") || tag == QLatin1String("p:pic")
                || tag == QLatin1String("p:graphicFrame"))
            {
                // 位置：a:off x/y + a:ext cx/cy（用来近似阅读顺序与图片尺寸）
                double x = 0.0, y = 0.0, cx = 0.0, cy = 0.0;
                std::function<void(const QDomElement &)> findXfrm = [&](const QDomElement &e) {
                    if (e.tagName() == QLatin1String("a:xfrm"))
                    {
                        const QDomElement off = e.firstChildElement(QStringLiteral("a:off"));
                        const QDomElement ext = e.firstChildElement(QStringLiteral("a:ext"));
                        x = attrOf(off, QStringLiteral("x")).toDouble();
                        y = attrOf(off, QStringLiteral("y")).toDouble();
                        cx = attrOf(ext, QStringLiteral("cx")).toDouble();
                        cy = attrOf(ext, QStringLiteral("cy")).toDouble();
                        return;
                    }
                    for (QDomElement c = e.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
                        findXfrm(c);
                };
                findXfrm(el);

                // 是否标题占位符
                bool isTitle = false;
                std::function<void(const QDomElement &)> findPh = [&](const QDomElement &e) {
                    if (e.tagName() == QLatin1String("p:ph"))
                    {
                        const QString type = e.attribute(QStringLiteral("type"));
                        if (type == QLatin1String("title") || type == QLatin1String("ctrTitle"))
                            isTitle = true;
                        return;
                    }
                    for (QDomElement c = e.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
                        findPh(c);
                };
                findPh(el);

                if (tag == QLatin1String("p:pic"))
                {
                    std::function<void(const QDomElement &)> findBlip = [&](const QDomElement &e) {
                        if (e.tagName() == QLatin1String("a:blip"))
                        {
                            const QString rid = e.attribute(QStringLiteral("r:embed"));
                            const QString target = slideRelTargets.value(rid);
                            if (!target.isEmpty())
                            {
                                const QString mediaPath = normalizePath(slideDir, target);
                                Block b;
                                b.order = y * 100000.0 + x;
                                addImage(m_zip.entry(mediaPath), cx / kEmuPerPt, cy / kEmuPerPt, &b);
                                if (!b.imageUrl.isEmpty()
                                    && !chargeDecodeBudget(&decodedBytes, m_images.value(b.imageUrl)))
                                {
                                    // 超整份文档的累计解码预算：丢弃该图，文档其余内容照常显示
                                    m_images.remove(b.imageUrl);
                                    b.imageUrl.clear();
                                    m_truncated = true;
                                }
                                if (!b.imageUrl.isEmpty()) slide.blocks.append(b);
                            }
                            return;
                        }
                        for (QDomElement c = e.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
                            findBlip(c);
                    };
                    findBlip(el);
                }
                else
                {
                    // 文本框：p:txBody/a:p（段落），每个段落一个 Block
                    std::function<void(const QDomElement &)> findTxBody = [&](const QDomElement &e) {
                        if (e.tagName() == QLatin1String("p:txBody"))
                        {
                            for (QDomElement p = e.firstChildElement(QStringLiteral("a:p")); !p.isNull();
                                 p = p.nextSiblingElement(QStringLiteral("a:p")))
                            {
                                QString text;
                                double sizePt = 0.0;
                                bool   bold = false;
                                for (QDomElement r = p.firstChildElement(); !r.isNull();
                                     r = r.nextSiblingElement())
                                {
                                    if (r.tagName() == QLatin1String("a:r"))
                                    {
                                        const QDomElement rPr = r.firstChildElement(QStringLiteral("a:rPr"));
                                        if (!rPr.isNull())
                                        {
                                            const double sz = attrOf(rPr, QStringLiteral("sz")).toDouble();
                                            if (sz > 0.0) sizePt = sz / 100.0;
                                            if (attrOf(rPr, QStringLiteral("b")) == QLatin1String("1"))
                                                bold = true;
                                        }
                                        const QDomElement t = r.firstChildElement(QStringLiteral("a:t"));
                                        if (!t.isNull()) text += t.text();
                                    }
                                    else if (r.tagName() == QLatin1String("a:br"))
                                    {
                                        text += QLatin1Char('\n');
                                    }
                                }
                                if (text.trimmed().isEmpty()) continue;
                                if (slide.blocks.size() >= kMaxBlocks) { m_truncated = true; break; }
                                Block b;
                                b.text = text;
                                b.sizePt = sizePt;
                                b.bold = bold;
                                b.title = isTitle;
                                b.order = y * 100000.0 + x;
                                slide.blocks.append(b);
                            }
                            return;
                        }
                        for (QDomElement c = e.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
                            findTxBody(c);
                    };
                    findTxBody(el);
                }
                return;
            }

            for (QDomElement c = el.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
                walk(c);
        };
        walk(slideDoc.documentElement());

        std::stable_sort(slide.blocks.begin(), slide.blocks.end(),
                         [](const Block &a, const Block &b) { return a.order < b.order; });
        m_slides.append(slide);
    }

    m_ok = !m_slides.isEmpty();
    if (!m_ok) return fail(QStringLiteral("没有可显示的幻灯片内容。"));
    return true;
}

bool SlidesDocument::parseOdp(QString *error)
{
    m_kind = QStringLiteral("odp");
    auto fail = [&](const QString &msg) {
        m_ok = false;
        if (error) *error = msg;
        return false;
    };

    const QByteArray odpXml = m_zip.entry(QStringLiteral("content.xml"));
    if (!xmlWithinTagBudget(odpXml))
        return fail(QStringLiteral("ODP XML 元素过多。"));
    QDomDocument doc;
    if (!doc.setContent(odpXml))
        return fail(QStringLiteral("无法解析 ODP 的 content.xml。"));

    QVector<QDomElement> pages;
    std::function<void(const QDomElement &)> findPages = [&](const QDomElement &el) {
        if (pages.size() >= kMaxSlides) return;
        if (el.tagName() == QLatin1String("draw:page")) { pages.append(el); return; }
        for (QDomElement c = el.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
            findPages(c);
    };
    findPages(doc.documentElement());
    if (pages.isEmpty())
        return fail(QStringLiteral("ODP 里找不到 draw:page。"));

    // 整份文档的累计解码预算（像素估算）：超预算的图片不再加入文档
    qint64 decodedBytes = 0;

    for (const QDomElement &page : pages)
    {
        Slide slide;

        for (QDomElement frame = page.firstChildElement(QStringLiteral("draw:frame")); !frame.isNull();
             frame = frame.nextSiblingElement(QStringLiteral("draw:frame")))
        {
            if (slide.blocks.size() >= kMaxBlocks) { m_truncated = true; break; }

            double wPt = 0.0, hPt = 0.0;
            // svg:width/height 是长度值（数字 + 可选单位）。先把单位后缀摘下来再解析数字，
            // 再按单位换算成 pt；原实现先剥后缀再 endsWith 判断，换算分支恒假（10cm 被当 10pt）。
            auto lenPt = [](const QString &v) -> double {
                QString s = v.trimmed().toLower();
                qsizetype unitStart = s.size();
                while (unitStart > 0)
                {
                    const QChar ch = s.at(unitStart - 1);
                    if (!ch.isLetter() && ch != QLatin1Char('%')) break;
                    --unitStart;
                }
                const QString unit = s.mid(unitStart);
                s.truncate(unitStart);

                bool ok = false;
                const double n = s.trimmed().toDouble(&ok);
                if (!ok || !qIsFinite(n)) return 0.0;              // 非法/空值交给调用方回退
                if (unit == QLatin1String("cm")) return n * 72.0 / 2.54;
                if (unit == QLatin1String("mm")) return n * 72.0 / 25.4;
                if (unit == QLatin1String("in")) return n * 72.0;
                if (unit == QLatin1String("px")) return n * 72.0 / 96.0;
                if (unit.isEmpty() || unit == QLatin1String("pt")) return n;
                return 0.0;                                        // 未知单位（% / em 等）按非法处理
            };
            wPt = lenPt(frame.attribute(QStringLiteral("svg:width")));
            hPt = lenPt(frame.attribute(QStringLiteral("svg:height")));

            // 图片
            for (QDomElement img = frame.firstChildElement(QStringLiteral("draw:image")); !img.isNull();
                 img = img.nextSiblingElement(QStringLiteral("draw:image")))
            {
                QString href = img.attribute(QStringLiteral("xlink:href")).trimmed();
                while (href.startsWith(QLatin1String("./"))) href.remove(0, 2);
                if (href.isEmpty() || href.contains(QLatin1String("://"))) continue;
                Block b;
                addImage(m_zip.entry(href), wPt, hPt, &b);
                if (!b.imageUrl.isEmpty()
                    && !chargeDecodeBudget(&decodedBytes, m_images.value(b.imageUrl)))
                {
                    // 超整份文档的累计解码预算：丢弃该图，文档其余内容照常显示
                    m_images.remove(b.imageUrl);
                    b.imageUrl.clear();
                    m_truncated = true;
                }
                if (!b.imageUrl.isEmpty()) slide.blocks.append(b);
            }

            // 文本框
            for (QDomElement box = frame.firstChildElement(QStringLiteral("draw:text-box")); !box.isNull();
                 box = box.nextSiblingElement(QStringLiteral("draw:text-box")))
            {
                // 正文可能在 text:list/list-item、表格或 draw:g 内；递归收集段落，
                // 不再只取 text-box 的直接子 text:p。
                QVector<QDomElement> paras;
                std::function<void(const QDomElement &)> collectParas = [&](const QDomElement &parent) {
                    for (QDomElement c = parent.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
                    {
                        const QString tag = c.tagName();
                        if (tag == QLatin1String("text:p") || tag == QLatin1String("text:h"))
                            paras.append(c);
                        else
                            collectParas(c);
                    }
                };
                collectParas(box);
                for (const QDomElement &p : paras)
                {
                    const bool heading = (p.tagName() == QLatin1String("text:h"));
                    const QString text = p.text().trimmed();
                    if (text.isEmpty()) continue;
                    if (slide.blocks.size() >= kMaxBlocks) { m_truncated = true; break; }
                    Block b;
                    b.text = text;
                    b.title = heading;
                    b.sizePt = heading ? 20.0 : 0.0;
                    b.bold = heading;
                    slide.blocks.append(b);
                }
            }
        }

        m_slides.append(slide);
    }

    m_ok = !m_slides.isEmpty();
    if (!m_ok) return fail(QStringLiteral("没有可显示的幻灯片内容。"));
    return true;
}

SlidesDocument::RenderResult SlidesDocument::render(double fontScale) const
{
    RenderResult res;
    res.images = m_images;
    res.truncated = m_truncated;

    QStringList out;
    int pageNo = 0;
    for (const Slide &s : m_slides)
    {
        ++pageNo;
        out << QStringLiteral("<div style=\"border:1px solid #bbb;border-radius:6px;"
                              "padding:12px 16px;margin:0 0 16px 0;\">");
        out << QStringLiteral("<div style=\"font-size:%1pt;color:#888;margin-bottom:6px\">")
                   .arg(fmtNum(9.0 * fontScale))
            << QStringLiteral("第 %1 页 / 共 %2 页").arg(pageNo).arg(m_slides.size())
            << QStringLiteral("</div>");

        for (const Block &b : s.blocks)
        {
            if (!b.imageUrl.isEmpty())
            {
                const double w = b.imgW * fontScale;
                out << QStringLiteral("<div style=\"margin:8px 0\"><img src=\"")
                    << b.imageUrl << QStringLiteral("\" width=\"") << fmtNum(w)
                    << QStringLiteral("\"></div>");
                continue;
            }

            QString text = esc(b.text);
            text.replace(QLatin1String("\n"), QLatin1String("<br/>"));

            const double basePt = b.sizePt > 0.0 ? b.sizePt : (b.title ? 20.0 : 14.0);
            QStringList css;
            css << QStringLiteral("font-size:%1pt").arg(fmtNum(basePt * fontScale));
            css << QStringLiteral("margin:4px 0");
            if (b.title) css << QStringLiteral("font-weight:bold");
            out << QStringLiteral("<p style=\"") + css.join(QLatin1Char(';')) + QStringLiteral("\">")
                + (b.bold && !b.title ? QStringLiteral("<b>") : QString())
                + text
                + (b.bold && !b.title ? QStringLiteral("</b>") : QString())
                + QStringLiteral("</p>");
        }

        out << QStringLiteral("</div>");
    }

    if (m_truncated)
        out << QStringLiteral("<p><i>（演示文稿过大，仅显示前一部分内容）</i></p>");

    res.html = out.join(QString());
    return res;
}

#include "slidesdocument.h"

#include <QBuffer>
#include <QDomDocument>
#include <QDomElement>
#include <QHash>
#include <QDebug>
#include <QImageReader>
#include <QSize>
#include <QStringList>
#include <algorithm>
#include <functional>
#include "xmlguard.h"
#include "percentdecode.h"

namespace {

constexpr int    kMaxSlides   = 200;
constexpr int    kMaxBlocks   = 2000;
constexpr int    kMaxRelationships = 10000;   // 单个 .rels 条目上限（防 QDom 节点放大）
constexpr double kEmuPerPt    = 12700.0;      // 1pt = 12700 EMU
constexpr qint64 kMaxImageBytes = 32LL * 1024 * 1024;
constexpr qint64 kMaxDecodedImageBytes = 256LL * 1024 * 1024;   // 整份文档累计解码预算
constexpr qint64 kMaxSlidesTextChars = 20LL * 1000 * 1000;      // 整份文档累计正文字符
// P1-13：递归遍历的结构深度与单文档访问元素总量上限（与 xmlguard 的 50 万标签同量级）
constexpr int    kMaxXmlDepth     = 200;
constexpr int    kMaxVisitedNodes = 500000;
// P1-14：解码前的图片尺寸/内存预检（QImage 按 4 字节/像素估算解码后占用）
constexpr qint64 kMaxImagePixels       = 40000000LL;            // 40 MP
constexpr qint64 kMaxImageDecodeBytes  = 160LL * 1024 * 1024;   // 单图解码后约 160 MiB
constexpr qint64 kMaxImageDecodeNoSize = 32LL * 1024 * 1024;    // 读不到尺寸时只允许小文件解码

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
    // OPC 允许以 / 开头的包根绝对部件名；此时不能再拼 baseDir。
    const QString full = rel.startsWith(QLatin1Char('/'))
        ? rel
        : (baseDir + QLatin1Char('/') + rel);
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
    // 与其它 XML 部件同等的预算：rels 也可能被放大成几十万个节点。
    if (!xmlWithinTagBudget(relsXml, 20000))
    {
        // 正常 rels 只有几百条；放行到默认的 50 万标签会让 QDom 再放大成 GB 级
        qWarning() << "[Slides] rels XML 超过预算，已忽略";
        return targets;
    }
    QDomDocument doc;
    if (!doc.setContent(relsXml)) return targets;
    int nRel = 0;
    for (QDomElement e = doc.documentElement().firstChildElement(QStringLiteral("Relationship"));
         !e.isNull(); e = e.nextSiblingElement(QStringLiteral("Relationship")))
    {
        if (++nRel > kMaxRelationships)
        {
            qWarning() << "[Slides] rels 条目超过上限，已截断";
            break;
        }
        // OPC 的 Target 是 IRI：部件名含空格/非 ASCII 时写成 %XX，
        // 而 ZIP 条目名是解码后的字面值，查表前必须解码。
        targets.insert(e.attribute(QStringLiteral("Id")),
                       percentDecodeStrict(e.attribute(QStringLiteral("Target"))));
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

// ---- P1-14：解码前的图片尺寸/内存预检 ----
// 只限制压缩字节挡不住「解压炸弹」：几 KB 的 PNG 头可以声明几万×几万像素，
// QImage::fromData 会直接分配数 GB 像素。这里先用 QImageReader 只读元数据。
enum class ImageGate { Ok, NoSize, TooLarge };

ImageGate precheckImage(const QByteArray &bytes, QSize *size)
{
    if (size) *size = QSize();
    if (bytes.isEmpty()) return ImageGate::NoSize;

    QByteArray data = bytes;          // QByteArray 隐式共享：这里只读，不会复制内容
    QBuffer device(&data);
    if (!device.open(QIODevice::ReadOnly)) return ImageGate::NoSize;
    QImageReader reader(&device);
    reader.setDecideFormatFromContent(true);   // zip 条目名不可信，按内容判断格式
    if (!reader.canRead()) return ImageGate::NoSize;

    const QSize sz = reader.size();
    if (size) *size = sz;
    if (!sz.isValid() || sz.width() <= 0 || sz.height() <= 0) return ImageGate::NoSize;

    const qint64 pixels = qint64(sz.width()) * qint64(sz.height());
    if (pixels > kMaxImagePixels) return ImageGate::TooLarge;
    if (pixels * 4 > kMaxImageDecodeBytes) return ImageGate::TooLarge;
    return ImageGate::Ok;
}

// P1-15：媒体路径归一化成负缓存/正缓存用的键（统一小写，避免大小写差异绕过缓存）
QString mediaKeyOf(const QString &mediaPath)
{
    return mediaPath.trimmed().toLower();
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
    m_imageByKey.clear();
    m_failedImages.clear();
    m_kind.clear();
    m_imageSeq = 0;
    m_truncated = false;
    m_truncatedReason.clear();
    m_ok = false;
}

// 截断记账（保留第一次的原因，P1-9/P2-9：不再静默丢内容）
void SlidesDocument::markTruncated(const QString &reason)
{
    m_truncated = true;
    if (m_truncatedReason.isEmpty() && !reason.isEmpty()) m_truncatedReason = reason;
}

QString SlidesDocument::normalizePath(const QString &baseDir, const QString &rel) const
{
    return joinPath(baseDir, rel);
}

// P1-15：按 zip 内路径取图。命中缓存就直接复用（不再解压）；
// 命中负缓存直接放弃（不再解压/解码）；都没有才解压。重复引用的图片因此只付一次成本。
void SlidesDocument::addImageFromZip(const QString &mediaPath, double wPt, double hPt,
                                     Block *block, bool *pbNewDecoded)
{
    if (pbNewDecoded) *pbNewDecoded = false;
    if (block == nullptr || mediaPath.isEmpty()) return;

    const QString key = mediaKeyOf(mediaPath);
    if (key.isEmpty()) return;

    // 1) 已判定不可用：直接跳过（无效格式/超大/超预算/解压失败都记在这里）
    if (m_failedImages.contains(key)) return;

    // 2) 已解码过：复用 URL，不再 m_zip.entry()
    const auto hit = m_imageByKey.constFind(key);
    if (hit != m_imageByKey.constEnd())
    {
        const QString sExisting = hit.value();
        const QImage &img = m_images[sExisting];
        block->imageUrl = sExisting;
        const double w = qBound(1.0, wPt > 0.0 ? wPt : 240.0, 10000.0);
        const double h = qBound(1.0, hPt > 0.0 ? hPt
                                               : (w * img.height() / qMax(1, img.width())),
                                10000.0);
        block->imgW = w;
        block->imgH = h;
        return;
    }

    // 3) 缓存未命中：这时才解压（原实现在调用 addImage 之前就 m_zip.entry() 了）
    if (!m_zip.contains(mediaPath))
    {
        m_failedImages.insert(key);
        return;
    }
    const QByteArray bytes = m_zip.entry(mediaPath);
    if (bytes.isEmpty() || bytes.size() > kMaxImageBytes)
    {
        m_failedImages.insert(key);       // 解压失败或压缩字节超限
        markTruncated(QStringLiteral("内嵌图片体积过大或无法解压"));
        return;
    }
    addImage(bytes, wPt, hPt, block, key, pbNewDecoded);
}

void SlidesDocument::addImage(const QByteArray &bytes, double wPt, double hPt, Block *block,
                              const QString &sKey, bool *pbNewDecoded)
{
    if (pbNewDecoded) *pbNewDecoded = false;
    if (block == nullptr) return;

    const QString sCacheKey = mediaKeyOf(sKey);
    // 同一 zip 图片被多页/多次 a:blip 引用时复用已有 URL，不重复解码。
    if (!sCacheKey.isEmpty() && m_imageByKey.contains(sCacheKey))
    {
        const QString sExisting = m_imageByKey.value(sCacheKey);
        const QImage &img = m_images[sExisting];
        block->imageUrl = sExisting;
        const double w = qBound(1.0, wPt > 0.0 ? wPt : 240.0, 10000.0);
        const double h = qBound(1.0, hPt > 0.0 ? hPt
                                               : (w * img.height() / qMax(1, img.width())),
                                10000.0);
        block->imgW = w;
        block->imgH = h;
        return;
    }

    if (bytes.isEmpty() || bytes.size() > kMaxImageBytes)
    {
        if (!sCacheKey.isEmpty()) m_failedImages.insert(sCacheKey);
        return;
    }

    // ---- P1-14：解码前的尺寸/内存预检 ----
    QSize size;
    const ImageGate gate = precheckImage(bytes, &size);
    if (gate == ImageGate::TooLarge)
    {
        if (!sCacheKey.isEmpty()) m_failedImages.insert(sCacheKey);   // 负缓存
        markTruncated(QStringLiteral("图片像素超过上限"));
        return;
    }
    if (gate == ImageGate::NoSize && bytes.size() > kMaxImageDecodeNoSize)
    {
        if (!sCacheKey.isEmpty()) m_failedImages.insert(sCacheKey);
        markTruncated(QStringLiteral("图片无法预检尺寸且体积过大"));
        return;
    }
    if (gate == ImageGate::Ok)
    {
        const qint64 estimate = qint64(size.width()) * qint64(size.height()) * 4;
        if (estimate > kMaxImageDecodeBytes)
        {
            if (!sCacheKey.isEmpty()) m_failedImages.insert(sCacheKey);
            markTruncated(QStringLiteral("图片解码内存超过预算"));
            return;
        }
    }

    const QImage img = QImage::fromData(bytes);
    if (img.isNull())
    {
        // WMF/EMF 等 Qt 不能直接解码的格式：记入负缓存，不再重复尝试
        if (!sCacheKey.isEmpty()) m_failedImages.insert(sCacheKey);
        return;
    }

    const QString url = m_urlPrefix + QString::number(++m_imageSeq);
    m_images.insert(url, img);
    if (!sCacheKey.isEmpty()) m_imageByKey.insert(sCacheKey, url);
    if (pbNewDecoded) *pbNewDecoded = true;
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
    // P1-13：递归下钻带深度 + 访问元素总量上限
    int nVisited = 0;
    std::function<void(const QDomElement &, int)> findList = [&](const QDomElement &el, int nDepth) {
        if (!sldIdLst.isNull()) return;
        if (nDepth > kMaxXmlDepth) { markTruncated(QStringLiteral("文档结构过深")); return; }
        if (++nVisited > kMaxVisitedNodes) { markTruncated(QStringLiteral("文档结构过大")); return; }
        if (el.tagName() == QLatin1String("p:sldIdLst")) { sldIdLst = el; return; }
        for (QDomElement c = el.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
            findList(c, nDepth + 1);
    };
    findList(pres.documentElement(), 0);

    // 先按 kMaxSlides 截断再查表：畸形文档的 sldId 数量不受限，逐个处理会拖死 UI
    int sldIdCount = 0;
    for (QDomElement e = sldIdLst.firstChildElement(QStringLiteral("p:sldId")); !e.isNull();
         e = e.nextSiblingElement(QStringLiteral("p:sldId")))
    {
        if (++sldIdCount > kMaxSlides) { markTruncated(QStringLiteral("幻灯片数量超过上限")); break; }
        const QString target = presRelTargets.value(e.attribute(QStringLiteral("r:id")));
        if (target.isEmpty()) continue;
        const bool bAbsoluteTarget = target.trimmed().startsWith(QLatin1Char('/'));
        QString path = normalizePath(QStringLiteral("ppt"), target);
        if (!bAbsoluteTarget && !path.startsWith(QLatin1String("ppt/")))
            path = QStringLiteral("ppt/") + path;
        // 同一个 slide XML 被多个 sldId 引用时只解析一次（否则 200 个 sldId
        // 可重复解压/解析同一份 64MiB slide）。
        if (!slidePaths.contains(path))
            slidePaths.append(path);
    }
    if (slidePaths.isEmpty())
        return fail(QStringLiteral("演示文稿里没有任何幻灯片。"));

    // 整份文档的累计解码预算（像素估算）：超预算的图片不再加入文档
    qint64 decodedBytes = 0;
    qint64 totalTextChars = 0;

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
        // P1-13：主遍历带结构深度与访问元素总量上限；find* 下钻只加深度限制
        // （它们的搜索范围就是一个形状的子树，已随 walk 计过数）。
        std::function<void(const QDomElement &, int)> walk = [&](const QDomElement &el, int nDepth) {
            if (slide.blocks.size() >= kMaxBlocks) { markTruncated(QStringLiteral("单页内容块超过上限")); return; }
            if (nDepth > kMaxXmlDepth) { markTruncated(QStringLiteral("文档结构过深")); return; }
            if (++nVisited > kMaxVisitedNodes) { markTruncated(QStringLiteral("文档结构过大")); return; }

            const QString tag = el.tagName();
            if (tag == QLatin1String("p:sp") || tag == QLatin1String("p:pic")
                || tag == QLatin1String("p:graphicFrame"))
            {
                // 位置：a:off x/y + a:ext cx/cy（用来近似阅读顺序与图片尺寸）
                double x = 0.0, y = 0.0, cx = 0.0, cy = 0.0;
                std::function<void(const QDomElement &, int)> findXfrm =
                    [&](const QDomElement &e, int d) {
                    if (d > kMaxXmlDepth) return;
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
                        findXfrm(c, d + 1);
                };
                findXfrm(el, 0);

                // 是否标题占位符
                bool isTitle = false;
                std::function<void(const QDomElement &, int)> findPh =
                    [&](const QDomElement &e, int d) {
                    if (d > kMaxXmlDepth) return;
                    if (e.tagName() == QLatin1String("p:ph"))
                    {
                        const QString type = e.attribute(QStringLiteral("type"));
                        if (type == QLatin1String("title") || type == QLatin1String("ctrTitle"))
                            isTitle = true;
                        return;
                    }
                    for (QDomElement c = e.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
                        findPh(c, d + 1);
                };
                findPh(el, 0);

                if (tag == QLatin1String("p:pic"))
                {
                    std::function<void(const QDomElement &, int)> findBlip =
                        [&](const QDomElement &e, int d) {
                        // 达到块预算后不再解压后续图片：只挡 append 的话，
                        // 重复引用仍会反复 inflate 同一图片。
                        if (slide.blocks.size() >= kMaxBlocks) { markTruncated(QStringLiteral("单页内容块超过上限")); return; }
                        if (d > kMaxXmlDepth) { markTruncated(QStringLiteral("文档结构过深")); return; }
                        if (e.tagName() == QLatin1String("a:blip"))
                        {
                            const QString rid = e.attribute(QStringLiteral("r:embed"));
                            const QString target = slideRelTargets.value(rid);
                            if (!target.isEmpty())
                            {
                                const QString mediaPath = normalizePath(slideDir, target);
                                Block b;
                                b.order = y * 100000.0 + x;
                                bool bNewDecoded = false;
                                // P1-15：这里只传路径，缓存/负缓存都在 addImageFromZip 里先判断，
                                // 命中时不再 m_zip.entry()（原实现无条件先解压再查缓存）。
                                addImageFromZip(mediaPath, cx / kEmuPerPt, cy / kEmuPerPt,
                                                &b, &bNewDecoded);
                                if (bNewDecoded && !b.imageUrl.isEmpty()
                                    && !chargeDecodeBudget(&decodedBytes, m_images.value(b.imageUrl)))
                                {
                                    // 超整份文档的累计解码预算：丢弃新图；缓存命中的共享图不能删。
                                    // P1-15：同时记入负缓存，后续引用不再解压/解码。
                                    m_images.remove(b.imageUrl);
                                    m_imageByKey.remove(mediaKeyOf(mediaPath));
                                    m_failedImages.insert(mediaKeyOf(mediaPath));
                                    b.imageUrl.clear();
                                    markTruncated(QStringLiteral("图片解码内存超过预算"));
                                }
                                if (!b.imageUrl.isEmpty())
                                {
                                    // 图片分支同样受块预算约束：同一 rId 重复引用时
                                    // 缓存不再解码、也不消耗解码预算，但仍会每引用一次追加一个 Block。
                                    if (slide.blocks.size() >= kMaxBlocks) { markTruncated(QStringLiteral("单页内容块超过上限")); return; }
                                    slide.blocks.append(b);
                                }
                            }
                            return;
                        }
                        for (QDomElement c = e.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
                            findBlip(c, d + 1);
                    };
                    findBlip(el, 0);
                }
                else
                {
                    // 文本框：p:txBody/a:p（段落），每个段落一个 Block
                    std::function<void(const QDomElement &, int)> findTxBody =
                        [&](const QDomElement &e, int d) {
                        if (d > kMaxXmlDepth) { markTruncated(QStringLiteral("文档结构过深")); return; }
                        if (e.tagName() == QLatin1String("p:txBody") || e.tagName() == QLatin1String("a:txBody"))
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
                                if (slide.blocks.size() >= kMaxBlocks) { markTruncated(QStringLiteral("单页内容块超过上限")); break; }
                                totalTextChars += text.size();
                                if (totalTextChars > kMaxSlidesTextChars) { markTruncated(QStringLiteral("正文字符总数超过上限")); break; }
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
                            findTxBody(c, d + 1);
                    };
                    findTxBody(el, 0);
                }
                return;
            }

            for (QDomElement c = el.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
                walk(c, nDepth + 1);
        };
        walk(slideDoc.documentElement(), 0);

        std::stable_sort(slide.blocks.begin(), slide.blocks.end(),
                         [](const Block &a, const Block &b) { return a.order < b.order; });
        m_slides.append(slide);
        // P1-13：整份文档的元素预算已用尽，后续页不再解析（避免一串空白页）
        if (nVisited > kMaxVisitedNodes) break;
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

    // P1-13：递归遍历带深度 + 访问元素总量上限（整份 ODP 共用一个计数）
    int nVisited = 0;
    QVector<QDomElement> pages;
    std::function<void(const QDomElement &, int)> findPages = [&](const QDomElement &el, int nDepth) {
        if (pages.size() >= kMaxSlides) return;
        if (nDepth > kMaxXmlDepth) { markTruncated(QStringLiteral("文档结构过深")); return; }
        if (++nVisited > kMaxVisitedNodes) { markTruncated(QStringLiteral("文档结构过大")); return; }
        if (el.tagName() == QLatin1String("draw:page")) { pages.append(el); return; }
        for (QDomElement c = el.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
            findPages(c, nDepth + 1);
    };
    findPages(doc.documentElement(), 0);
    if (pages.isEmpty())
        return fail(QStringLiteral("ODP 里找不到 draw:page。"));

    // 整份文档的累计解码/文本预算：超预算的内容不再加入文档
    qint64 decodedBytes = 0;
    qint64 totalTextChars = 0;

    for (const QDomElement &page : pages)
    {
        Slide slide;

        for (QDomElement frame = page.firstChildElement(QStringLiteral("draw:frame")); !frame.isNull();
             frame = frame.nextSiblingElement(QStringLiteral("draw:frame")))
        {
            if (slide.blocks.size() >= kMaxBlocks) { markTruncated(QStringLiteral("单页内容块超过上限")); break; }
            if (++nVisited > kMaxVisitedNodes) { markTruncated(QStringLiteral("文档结构过大")); break; }

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
                if (slide.blocks.size() >= kMaxBlocks) { markTruncated(QStringLiteral("单页内容块超过上限")); break; }
                QString href = img.attribute(QStringLiteral("xlink:href")).trimmed();
                while (href.startsWith(QLatin1String("./"))) href.remove(0, 2);
                // ODF 的 xlink:href 是 IRI：空格/非 ASCII 写成 %XX，
                // 而 ZIP 条目名是解码后的字面值（"Pictures/图 1.png"）。
                href = percentDecodeStrict(href);
                if (href.isEmpty() || href.contains(QLatin1String("://"))) continue;
                Block b;
                bool bNewDecoded = false;
                // P1-15：先查缓存/负缓存，未命中才解压
                addImageFromZip(href, wPt, hPt, &b, &bNewDecoded);
                if (bNewDecoded && !b.imageUrl.isEmpty()
                    && !chargeDecodeBudget(&decodedBytes, m_images.value(b.imageUrl)))
                {
                    // 超整份文档的累计解码预算：丢弃新图；缓存命中的共享图不能删。
                    // P1-15：记入负缓存，后续引用不再解压/解码。
                    m_images.remove(b.imageUrl);
                    m_imageByKey.remove(mediaKeyOf(href));
                    m_failedImages.insert(mediaKeyOf(href));
                    b.imageUrl.clear();
                    markTruncated(QStringLiteral("图片解码内存超过预算"));
                }
                if (!b.imageUrl.isEmpty())
                {
                    if (slide.blocks.size() >= kMaxBlocks) { markTruncated(QStringLiteral("单页内容块超过上限")); break; }
                    slide.blocks.append(b);
                }
            }

            // 文本框
            for (QDomElement box = frame.firstChildElement(QStringLiteral("draw:text-box")); !box.isNull();
                 box = box.nextSiblingElement(QStringLiteral("draw:text-box")))
            {
                // 正文可能在 text:list/list-item、表格或 draw:g 内；递归收集段落，
                // 不再只取 text-box 的直接子 text:p。
                QVector<QDomElement> paras;
                std::function<void(const QDomElement &, int)> collectParas =
                    [&](const QDomElement &parent, int nDepth) {
                    if (nDepth > kMaxXmlDepth) { markTruncated(QStringLiteral("文档结构过深")); return; }
                    if (++nVisited > kMaxVisitedNodes) { markTruncated(QStringLiteral("文档结构过大")); return; }
                    for (QDomElement c = parent.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
                    {
                        const QString tag = c.tagName();
                        if (tag == QLatin1String("text:p") || tag == QLatin1String("text:h"))
                            paras.append(c);
                        else
                            collectParas(c, nDepth + 1);
                    }
                };
                collectParas(box, 0);
                for (const QDomElement &p : paras)
                {
                    const bool heading = (p.tagName() == QLatin1String("text:h"));
                    const QString text = p.text().trimmed();
                    if (text.isEmpty()) continue;
                    if (slide.blocks.size() >= kMaxBlocks) { markTruncated(QStringLiteral("单页内容块超过上限")); break; }
                    totalTextChars += text.size();
                    if (totalTextChars > kMaxSlidesTextChars) { markTruncated(QStringLiteral("正文字符总数超过上限")); break; }
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
        // P1-13：整份文档的元素预算已用尽，后续页不再解析（避免一串空白页）
        if (nVisited > kMaxVisitedNodes) break;
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
    {
        // P1-9 / P2-9：把触发原因（页数/内容块/图片/结构上限）写进最终 HTML，
        // 而不是只置一个上层看不到的布尔量
        const QString why = m_truncatedReason.isEmpty()
                ? QStringLiteral("演示文稿过大") : m_truncatedReason;
        out << QStringLiteral("<p><i>（%1，仅显示前一部分内容）</i></p>").arg(esc(why));
    }

    res.html = out.join(QString());
    return res;
}

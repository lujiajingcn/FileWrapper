#ifndef SLIDESDOCUMENT_H
#define SLIDESDOCUMENT_H

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QMap>
#include <QString>
#include <QVector>

#include "zipreader.h"

// ============================================================================
// SlidesDocument：把 .pptx（OOXML PresentationML）与 .odp（OpenDocument
// Presentation）的**静态内容**转成 HTML（每页一个区块：标题 + 正文 + 图片）。
//
// pptx：
//   ppt/presentation.xml              -> p:sldIdLst（页序，r:id）
//   ppt/_rels/presentation.xml.rels   -> r:id -> slides/slideN.xml
//   ppt/slides/slideN.xml             -> p:sp（文本框）/ p:pic（图片）
//   ppt/slides/_rels/slideN.xml.rels  -> 图片 r:embed -> ../media/imageN.png
// odp：
//   content.xml -> draw:page（页序）-> draw:frame（draw:text-box / draw:image）
//
// 只做"看得到内容"：不还原版式坐标/主题色/动画/切换，表格按文本行展示。
// ============================================================================
class SlidesDocument
{
public:
    struct RenderResult {
        QString html;
        QMap<QString, QImage> images;   // 图片 URL -> 图像（resourceProvider 用）
        bool truncated = false;
    };

    bool open(const QByteArray &data, QString *error = nullptr);
    void reset();
    bool isOpen() const { return m_ok; }

    RenderResult render(double fontScale = 1.0) const;

    QString kind() const { return m_kind; }       // "pptx" / "odp"
    int slideCount() const { return m_slides.size(); }

private:
    struct Block {
        QString text;
        double  sizePt = 0.0;
        bool    bold   = false;
        bool    title  = false;
        QString imageUrl;             // 非空表示图片块
        double  imgW = 0.0;           // 图片显示宽（pt）
        double  imgH = 0.0;
        double  order = 0.0;          // 排序用（y*100000 + x）
    };
    struct Slide {
        QVector<Block> blocks;
    };

    bool parsePptx(QString *error);
    bool parseOdp(QString *error);
    void addImage(const QByteArray &bytes, double wPt, double hPt, Block *block,
                  const QString &sKey = QString(), bool *pbNewDecoded = nullptr);
    QString normalizePath(const QString &baseDir, const QString &rel) const;

    ZipReader        m_zip;
    QVector<Slide>   m_slides;
    QMap<QString, QImage> m_images;   // URL -> 图像
    QHash<QString, QString> m_imageByKey; // zip 内路径 -> URL（重复引用去重）
    QString          m_kind;
    int              m_imageSeq = 0;
    QString          m_urlPrefix = QStringLiteral("simg://slide/");
    bool             m_truncated = false;
    bool             m_ok = false;
};

#endif // SLIDESDOCUMENT_H

// 把 IconMCP 下载的 SVG 渲染成 Qt 资源用的 PNG。
//
// 单个输出：
//   svg2png --in icon.svg --out icon_24.png --size 24 [--color "#374151"] [--bg "#ffffff"]
// 批量输出（一次启动渲染所有尺寸）：
//   svg2png --in icon.svg --outdir png --prefix merge --sizes 20,24,32,40,48,64 --color "#374151"
//
// SVG 里的 currentColor 会被替换成 --color；未指定时用 #374151（与浅色主题正文同色）。
// 输出始终是 ARGB32 PNG（透明底），除非给 --bg。

#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>
#include <QDir>
#include <QFile>
#include <QTextStream>

static QString argValue(const QStringList &args, const QString &name, const QString &def = QString())
{
    const int i = args.indexOf(name);
    if (i >= 0 && i + 1 < args.size())
        return args.at(i + 1);
    return def;
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    const QStringList args = app.arguments();
    const QString inPath = argValue(args, "--in");
    const QString outPath = argValue(args, "--out");
    const QString outDir = argValue(args, "--outdir");
    const QString prefix = argValue(args, "--prefix");
    const QString sizesArg = argValue(args, "--sizes", argValue(args, "--size", "24"));
    const QString color = argValue(args, "--color", "#374151");
    const QString bg = argValue(args, "--bg");

    QList<int> sizes;
    for (const QString &s : sizesArg.split(QLatin1Char(','), Qt::SkipEmptyParts))
        sizes.append(s.trimmed().toInt());
    sizes.removeAll(0);

    if (inPath.isEmpty() || sizes.isEmpty()
        || ((outPath.isEmpty()) == (outDir.isEmpty() || prefix.isEmpty()))) {
        QTextStream(stderr) << "usage: svg2png --in icon.svg (--out file.png | --outdir dir --prefix name) "
                               "--sizes 20,24 [--color #RRGGBB] [--bg #RRGGBB]\n";
        return 2;
    }

    QFile f(inPath);
    if (!f.open(QIODevice::ReadOnly)) {
        QTextStream(stderr) << "cannot open " << inPath << "\n";
        return 3;
    }
    QString svg = QString::fromUtf8(f.readAll());
    svg.replace(QLatin1String("currentColor"), color);

    QSvgRenderer renderer(svg.toUtf8());
    if (!renderer.isValid()) {
        QTextStream(stderr) << "invalid SVG: " << inPath << "\n";
        return 4;
    }

    if (!outDir.isEmpty())
        QDir().mkpath(outDir);

    for (int size : sizes) {
        QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
        img.fill(bg.isEmpty() ? Qt::transparent : QColor(bg));

        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        renderer.render(&p, QRectF(0, 0, size, size));
        p.end();

        const QString target = outPath.isEmpty()
                ? QStringLiteral("%1/%2_%3.png").arg(outDir, prefix).arg(size)
                : outPath;
        if (!img.save(target, "PNG")) {
            QTextStream(stderr) << "cannot save " << target << "\n";
            return 5;
        }
        if (!outPath.isEmpty())
            break;
    }
    return 0;
}

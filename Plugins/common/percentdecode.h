#ifndef PERCENTDECODE_H
#define PERCENTDECODE_H

#include <QByteArray>
#include <QString>

// OPC / ODF 的部件名是 IRI：Target / xlink:href 里的空格、非 ASCII 会写成 %XX，
// 而 ZIP 条目名是解码后的字面值。这里只解码合法的 %HH 三元组，非法或字面的 '%'
// 原样保留——QUrl::fromPercentEncoding() 是容错解码，会把 "100%.png" 这类
// 未转义的文件名解成乱码，导致本来能命中的条目查不到。
inline QString percentDecodeStrict(const QString &s)
{
    const QByteArray in = s.toUtf8();
    QByteArray out;
    out.reserve(in.size());
    const auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (int i = 0; i < in.size(); ++i)
    {
        const char c = in.at(i);
        if (c == '%' && i + 2 < in.size())
        {
            const int hi = hexVal(in.at(i + 1));
            const int lo = hexVal(in.at(i + 2));
            if (hi >= 0 && lo >= 0)
            {
                out.append(char(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.append(c);
    }
    return QString::fromUtf8(out);
}

#endif // PERCENTDECODE_H

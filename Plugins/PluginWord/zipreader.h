#ifndef ZIPREADER_H
#define ZIPREADER_H

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

// 自包含的最小 ZIP 读取器（替代 Qt 私有类 QZipReader）。
//
// 与 Plugins/PluginBook/zipreader.* 是同一份实现（各插件保持独立可编译，
// 不引入跨插件依赖）；改这里的问题时请同步另一份。
//
// - 直接解析中央目录（central directory），按条目名做归一化匹配，
//   因此不受条目名是否带前导 '/'、'./' 或反斜杠的影响。
// - 压缩数据用 zlib 原生 inflate（windowBits=-MAX_WBITS 即裸 deflate，正是 ZIP 所用）。
// - 不依赖任何 Qt 私有头，消除 "tied to this specific Qt build" 的构建告警与版本耦合。
// - 加密条目（general purpose flag bit0）不收录：密文无法解压。

class ZipReader
{
public:
    struct Entry
    {
        QString name;      // 已归一化：去掉前导 './' 与 '/'，反斜杠转 '/'
        int     method;    // 0=存储, 8=deflate
        qint64  compSize;
        qint64  uncompSize;
        qint64  localOff;  // 本地文件头偏移
    };

    bool open(const QByteArray &data);
    bool isValid() const { return m_ok; }

    QStringList entryNames() const;
    bool        contains(const QString &name) const;   // 归一化后不区分大小写匹配
    QByteArray  entry(const QString &name) const;       // 归一化匹配并解压

private:
    static QString normalize(const QString &p);
    QByteArray inflate(const QByteArray &comp, qint64 uncompSize) const;

    QByteArray  m_data;
    QList<Entry> m_entries;
    bool        m_ok = false;
};

#endif // ZIPREADER_H

#ifndef ZIPREADER_H
#define ZIPREADER_H

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QList>

// 自包含的最小 ZIP 读取器（替代 Qt 私有类 QZipReader）。
//
// 位置：Plugins/common/ —— 由多个插件**共享编译**（PluginBook 解 EPUB、
// PluginWord 解 .docx），通过 plugin_common.pri 的 PLUGIN_SHARED_SOURCES
// 各自编进自己的 DLL。它是源码级共享，不是运行时共享库：每个插件仍然
// 独立可编译、可部署、可单独禁用，不会产生跨插件依赖。
// 改动此处会同时影响两个插件，务必两边都验证（epub + docx）。
//
// - 直接解析中央目录（central directory），按条目名做归一化匹配，
//   因此不受条目名是否带前导 '/'、'./' 或反斜杠的影响（QZipReader 的 fileData
//   用原始名匹配，遇到带前导 '/' 的条目会返回空，导致 EPUB 解析失败）。
// - 加密条目（general purpose flag bit0）不解压，直接视为缺失。
// - 压缩数据用 zlib 原生 inflate（windowBits=-MAX_WBITS 即裸 deflate，正是 ZIP 所用）。
// - 不依赖任何 Qt 私有头，消除 "tied to this specific Qt build" 的构建告警与版本耦合。

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

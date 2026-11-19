#ifndef ZIPREADER_H
#define ZIPREADER_H

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QList>

// 自包含的最小 ZIP 读取器（替代 Qt 私有类 QZipReader）。
//
// 位置：Plugins/common/ —— 由多个插件**共享编译**（PluginBook 解 EPUB、
// PluginWord 解 .docx/.odt、PluginSheet、PluginSlides），通过
// plugin_common.pri 的 PLUGIN_SHARED_SOURCES 各自编进自己的 DLL。
// 它是源码级共享，不是运行时共享库：每个插件仍然独立可编译、可部署、
// 可单独禁用，不会产生跨插件依赖。改动此处必须同时验证所有使用方。
//
// - 直接解析中央目录（central directory），按条目名做归一化匹配，
//   因此不受条目名是否带前导 '/'、'./' 或反斜杠的影响。
// - 加密条目（general purpose flag bit0）不解压，直接视为缺失。
// - 压缩数据用 zlib 原生 inflate（windowBits=-MAX_WBITS 即裸 deflate）。
// - 不依赖任何 Qt 私有头。
//
// 资源预算（P1-12）：每个 ZipReader 实例自带 ParseBudget。stored 与 deflate
// 走同一个入口，统一限制「单条实际输出 / 全文档累计解压 / 提取次数」，
// 不再依赖各调用点自觉检查；条目声明的 CRC32 也会在返回前核对（P2-3）。

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
        quint32 crc32;     // 中央目录声明的内容 CRC32（P2-3）
    };

    // 每次 open 独立的解析预算。默认值足以打开常见文档，又不会让
    // 「多个高压缩 XML/图片部件」把进程内存与 CPU 吃穿。
    struct ParseBudget
    {
        qint64 maxEntryBytes   = 64LL * 1024 * 1024;   // 单条实际解压上限
        qint64 maxTotalBytes   = 256LL * 1024 * 1024;  // 全文档累计解压上限
        int    maxEntries      = 4096;                 // 中央目录条目上限
        int    maxExtractions  = 2048;                 // 单次会话允许的 entry() 次数
        qint64 totalBytes      = 0;                    // 已解压字节（含 stored）
        int    extractions     = 0;
        bool   exhausted       = false;                // 已因预算拒绝过请求
    };

    bool open(const QByteArray &data);
    bool isValid() const { return m_ok; }

    QStringList entryNames() const;
    bool        contains(const QString &name) const;   // 归一化后不区分大小写匹配
    QByteArray  entry(const QString &name) const;       // 归一化匹配并解压

    // 最近一次失败的原因（诊断用；成功时为空）
    QString lastError() const { return m_lastError; }
    // 预算可读/可调整：解析器可在打开前收紧上限
    ParseBudget &budget() { return m_budget; }
    const ParseBudget &budget() const { return m_budget; }

private:
    static QString normalize(const QString &p);
    QByteArray inflate(const QByteArray &comp, const Entry &e) const;
    bool chargeBudget(qint64 bytes, const QString &sName) const;

    QByteArray   m_data;
    QList<Entry> m_entries;
    bool         m_ok = false;
    mutable ParseBudget m_budget;
    mutable QString      m_lastError;
};

#endif // ZIPREADER_H

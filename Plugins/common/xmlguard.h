#pragma once

#include <QByteArray>

// 廉价 XML 放大防护：QDomDocument 会把每个元素物化成高开销节点，
// 4MB 的 <p/> 重复即可把工作集放大到数百 MB。这里在 setContent 之前
// 用一次线性扫描限制元素数量；合法文档远低于该阈值。
inline bool xmlWithinTagBudget(const QByteArray &data, int nMaxTags = 500000)
{
    if (data.size() > 64 * 1024 * 1024)
        return false;
    int nTags = 0;
    for (const char c : data)
    {
        if (c == '<' && ++nTags > nMaxTags)
            return false;
    }
    return true;
}

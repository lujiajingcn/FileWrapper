#pragma once

#include <QByteArray>

// 廉价 XML 放大防护：QDomDocument 会把每个元素物化成高开销节点，
// 4MB 的 <p/> 重复即可把工作集放大到数百 MB。这里在 setContent 之前
// 用一次线性扫描限制元素数量；合法文档远低于该阈值。
//
// 只数 '<' 挡不住属性节点：一个元素可以带几百万个唯一属性（XML 合法），
// QDom 会为每个属性建节点，且属性插入是二次方成本——2MB 的部件就能让
// GUI 线程卡住一分钟以上。因此这里同时限制「单个标签内的属性数」。
//
// 注释 / CDATA / 处理指令整体跳过（各按一个节点计数）：否则其中的引号会让
// 属性计数进入错误的引号状态，把后面的标签全部漏数，反而绕过预算。
inline bool xmlWithinTagBudget(const QByteArray &data, int nMaxTags = 500000)
{
    // 单元素属性上限：合法文档远低于此（rels 最多几十个，表格单元格 0~2 个）
    constexpr int kMaxAttributesPerTag = 4096;
    if (data.size() > 64 * 1024 * 1024)
        return false;

    const int n = int(data.size());
    auto matches = [&data, n](int pos, const char *lit, int len) -> bool {
        if (pos + len > n) return false;
        for (int k = 0; k < len; ++k)
            if (data.at(pos + k) != lit[k]) return false;
        return true;
    };

    int nTags = 0;
    int i = 0;
    while (i < n)
    {
        if (data.at(i) != '<')
        {
            ++i;
            continue;
        }

        // ---- 注释 / CDATA / 处理指令：整体跳到结束标记，各计一个节点 ----
        if (matches(i, "<!--", 4))
        {
            if (++nTags > nMaxTags) return false;
            const int end = data.indexOf("-->", i + 4);
            if (end < 0) return true;            // 未闭合：交给 QDom 报错
            i = end + 3;
            continue;
        }
        if (matches(i, "<![CDATA[", 9))
        {
            if (++nTags > nMaxTags) return false;
            const int end = data.indexOf("]]>", i + 9);
            if (end < 0) return true;
            i = end + 3;
            continue;
        }
        if (matches(i, "<?", 2))
        {
            if (++nTags > nMaxTags) return false;
            const int end = data.indexOf("?>", i + 2);
            if (end < 0) return true;
            i = end + 2;
            continue;
        }

        // ---- 普通标签：统计引号外的 '=' 作为属性数；'<' 也按节点计数 ----
        if (++nTags > nMaxTags) return false;
        int nAttrs = 0;
        char quote = 0;
        int j = i + 1;
        for (; j < n; ++j)
        {
            const char c = data.at(j);
            if (quote != 0)
            {
                if (c == quote) quote = 0;
                continue;
            }
            if (c == '"' || c == '\'') { quote = c; continue; }
            if (c == '<')
            {
                if (++nTags > nMaxTags) return false;
                continue;
            }
            if (c == '=' && ++nAttrs > kMaxAttributesPerTag)
                return false;
            if (c == '>') break;
        }
        if (j >= n) return true;                 // 未闭合：交给 QDom 报错
        i = j + 1;
    }
    return true;
}

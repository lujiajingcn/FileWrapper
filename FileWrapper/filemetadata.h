#ifndef FILEMETADATA_H
#define FILEMETADATA_H

#include <QString>
#include <QtGlobal>

// 文件系统元数据：合并只保存文件内容会丢掉「创建/修改/访问时间」和
// 只读/隐藏/系统/存档属性。FWDA v3 把这些值按条目写进头部，导出/分割时
// 再写回磁盘，属性对话框也直接显示它们。
//
// 约定：
//   * 时间是 UTC 毫秒时间戳（1970-01-01 起），0 = 未知/未记录；
//   * 属性沿用 Windows FILE_ATTRIBUTE_* 的位值，只保留 R/H/S/A 四位。
//     这样 Windows 上捕获、Windows 上恢复时语义一致；其它平台只映射只读位。
namespace FwMeta {

static const quint32 ATTR_READONLY = 0x00000001u;
static const quint32 ATTR_HIDDEN   = 0x00000002u;
static const quint32 ATTR_SYSTEM   = 0x00000004u;
static const quint32 ATTR_ARCHIVE  = 0x00000020u;
static const quint32 ATTR_MASK     = ATTR_READONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_ARCHIVE;

struct Metadata
{
    qint64  createdMs  = 0;
    qint64  modifiedMs = 0;
    qint64  accessedMs = 0;
    quint32 attributes = 0;
    bool    valid      = false;   // false = 没有可用的文件系统信息（文件不存在等）
};

// 读取磁盘文件的元数据。文件不存在/不是普通文件时返回 valid=false。
Metadata capture(const QString &sPath);

// 把元数据写回磁盘。只有非 0 的时间会被设置；attributes 为 0 时保持现有属性。
// 失败时返回 false 并在 psErr 给出可读原因（导出/分割只把它当警告，不删内容）。
bool apply(const QString &sPath, qint64 createdMs, qint64 modifiedMs,
           qint64 accessedMs, quint32 attributes, QString *psErr = nullptr);

// 属性显示文案：按 R/H/S/A 顺序拼字母；没有可显示位时返回 "—"。
QString attributesText(quint32 attributes);

// 时间戳是否在可显示范围内（0 表示未知，不算有效时间）。
inline bool isPlausibleTimestamp(qint64 ms) { return ms > 0 && ms <= 253402300799999LL; }

} // namespace FwMeta

#endif // FILEMETADATA_H

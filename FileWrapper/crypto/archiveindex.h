#ifndef FWCRYPTO_ARCHIVEINDEX_H
#define FWCRYPTO_ARCHIVEINDEX_H

// ============================================================
// FWDA v5 加密索引的明文序列化 / 解析
//
// 索引明文布局（见 archiveformat.h 顶部注释）：
//   u32 rootLen / root
//   u32 entryCount
//   entry × N:
//     u64 entryId / u64 size / u64 contentPos / u32 contentCrc
//     u64 createdMs / u64 modifiedMs / u64 accessedMs / u32 attributes
//     u32 chunkCount / u8 fileNonce[8] / u32 tagCount / u32 pathLen
//     path / tags
//   尾部补零到容量（预留区）
//
// 本模块只负责“明文 ↔ 结构”，不接触密钥；加解密在 archivecrypto。
// ============================================================

#include <QByteArray>
#include <QString>
#include <QVector>

#include "archiveformat.h"

namespace FwArcIndex {

// 精确索引字节数（不含预留）。
qint64 exactBytes(const QVector<FwArc::Entry> &entries, const QString &root,
                  QString *psErr = nullptr);

// 序列化为 capacity 字节（capacity ≥ exactBytes；尾部补零）。
// 条目里 entryId/fileNonce/chunkCount 必须已由调用方填好。
bool build(const QVector<FwArc::Entry> &entries, const QString &root,
           qint64 capacity, QByteArray *pOut, QString *psErr = nullptr);

// 解析索引明文；只做结构/字段校验，内容区越界与重叠由调用方按 headerSize/fileSize 校验。
bool parse(const QByteArray &plain, FwArc::Meta *pMeta, QString *psErr = nullptr);

} // namespace FwArcIndex

#endif // FWCRYPTO_ARCHIVEINDEX_H

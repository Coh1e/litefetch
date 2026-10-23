#pragma once
#include "ed2k_wire.h"
#include <QVector>
namespace downloader::ed2k {
inline constexpr quint64 blockSize=184320;
struct BlockRange {quint64 begin,end;}; // End is exclusive, as on the wire.
struct DataBlock {QByteArray fileHash;BlockRange range;QByteArray bytes;};
Packet requestBlocks(const QByteArray& fileHash,const QVector<BlockRange>& ranges,
    quint64 fileSize,bool allowLargeOffsets);
using DataBlockResult=std::variant<DataBlock,InvalidPacket>;
DataBlockResult parseDataBlock(const Packet& packet,const QByteArray& expectedHash,
    const QVector<BlockRange>& outstanding,quint64 fileSize);
}

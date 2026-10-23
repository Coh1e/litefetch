#include "ed2k_blocks.h"
#include "ed2k_hash.h"
#include <QtEndian>
#include <limits>
#include <stdexcept>
namespace downloader::ed2k {
namespace {
bool valid(BlockRange range,quint64 size){
    return range.end>range.begin&&range.end<=size&&range.end-range.begin<=blockSize
        &&range.begin/partSize==(range.end-1)/partSize;
}
template<class T>void append(QByteArray& output,T value){
    const auto position=output.size();output.resize(position+sizeof(T));qToLittleEndian<T>(value,output.data()+position);
}
}
Packet requestBlocks(const QByteArray& hash,const QVector<BlockRange>& ranges,quint64 size,bool largeAllowed){
    if(hash.size()!=16||ranges.isEmpty()||ranges.size()>3)throw std::invalid_argument("Invalid ed2k block request");
    bool large=false;
    for(qsizetype i=0;i<ranges.size();++i){
        const auto range=ranges[i];if(!valid(range,size))throw std::invalid_argument("Invalid ed2k block range");
        large|=range.end>std::numeric_limits<quint32>::max();
        for(qsizetype j=0;j<i;++j)if(range.begin<ranges[j].end&&ranges[j].begin<range.end)throw std::invalid_argument("Overlapping ed2k requests");
    }
    if(large&&!largeAllowed)throw std::invalid_argument("Peer did not negotiate large ed2k offsets");
    QByteArray payload=hash;
    for(int ends=0;ends<2;++ends)for(int index=0;index<3;++index){
        const quint64 value=index<ranges.size()?(ends?ranges[index].end:ranges[index].begin):0;
        if(large)append(payload,value);else append(payload,static_cast<quint32>(value));
    }
    return {large?Protocol::Emule:Protocol::Edonkey,static_cast<quint8>(large?0xa3:0x47),payload};
}
DataBlockResult parseDataBlock(const Packet& packet,const QByteArray& hash,const QVector<BlockRange>& outstanding,quint64 size){
    const bool large=packet.protocol==Protocol::Emule&&packet.opcode==0xa2;
    if(!(large||(packet.protocol==Protocol::Edonkey&&packet.opcode==0x46)))return InvalidPacket{QStringLiteral("不是未压缩ed2k数据块。")};
    const qsizetype header=large?32:24;
    if(hash.size()!=16||packet.payload.size()<header||packet.payload.first(16)!=hash)return InvalidPacket{QStringLiteral("ed2k数据块文件身份不匹配。")};
    const auto* data=packet.payload.constData()+16;
    const BlockRange range=large?BlockRange{qFromLittleEndian<quint64>(data),qFromLittleEndian<quint64>(data+8)}
        :BlockRange{qFromLittleEndian<quint32>(data),qFromLittleEndian<quint32>(data+4)};
    if(!valid(range,size)||range.end-range.begin!=quint64(packet.payload.size()-header))return InvalidPacket{QStringLiteral("ed2k数据块边界无效。")};
    bool requested=false;
    for(const auto& pending:outstanding)if(range.begin>=pending.begin&&range.end<=pending.end){requested=true;break;}
    if(!requested)return InvalidPacket{QStringLiteral("收到未请求的ed2k数据块。")};
    return DataBlock{hash,range,packet.payload.mid(header)};
}
}

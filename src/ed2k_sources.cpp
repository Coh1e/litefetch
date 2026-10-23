#include "ed2k_sources.h"
#include <QtEndian>
#include <QSet>
#include <limits>
#include <stdexcept>
namespace downloader::ed2k {
Packet requestSources(const QByteArray& hash,quint64 size,bool large){
    if(hash.size()!=16||!size)throw std::invalid_argument("Invalid source query file");
    QByteArray body=hash;char bytes[12];
    if(size>std::numeric_limits<quint32>::max()){
        if(!large)throw std::invalid_argument("Server has not negotiated large files");
        qToLittleEndian<quint32>(0,bytes);qToLittleEndian(size,bytes+4);body.append(bytes,12);
    }else{qToLittleEndian(quint32(size),bytes);body.append(bytes,4);}
    return {Protocol::Edonkey,0x19,body};
}
SourcesResult parseSources(const Packet& packet,const QByteArray& hash){
    const auto& p=packet.payload;
    if(packet.protocol!=Protocol::Edonkey||packet.opcode!=0x42||hash.size()!=16||p.size()<17||p.first(16)!=hash)
        return InvalidPacket{QStringLiteral("Invalid source reply identity or opcode")};
    const auto count=quint8(p[16]);
    if(p.size()!=17+6*count)return InvalidPacket{QStringLiteral("Invalid source reply length")};
    QVector<ServerSource> result;QSet<quint64> seen;
    for(int i=0;i<count;++i){const auto* bytes=p.constData()+17+i*6;
        const auto id=qFromLittleEndian<quint32>(bytes);const auto port=qFromLittleEndian<quint16>(bytes+4);
        if(!id||!port)continue;
        const auto key=(quint64(id)<<16)|port;
        if(!seen.contains(key)){seen.insert(key);result.append({id,port});}
    }
    return result;
}
}

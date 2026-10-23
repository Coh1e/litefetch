#include "ed2k_hashset.h"
#include "ed2k_hash.h"
#include <QtEndian>
namespace downloader::ed2k {
HashsetResult parseHashset(const Packet& packet,const QByteArray& hash,quint64 size){
    const auto expected=size<partSize?0:size/partSize+1;
    const auto& p=packet.payload;
    if(packet.protocol!=Protocol::Edonkey||packet.opcode!=0x52||hash.size()!=16||
       expected>65535||p.size()<18||p.first(16)!=hash)
        return InvalidPacket{QStringLiteral("Invalid hashset identity or opcode")};
    const auto count=qFromLittleEndian<quint16>(p.constData()+16);
    if(count!=expected||p.size()!=18+16*count)return InvalidPacket{QStringLiteral("Invalid hashset count or length")};
    QVector<QByteArray> hashes;
    if(count==0){hashes.append(hash);return hashes;} // Subpart identity is its part MD4.
    const auto body=QByteArrayView(p).sliced(18);
    if(QCryptographicHash::hash(body,QCryptographicHash::Md4)!=hash)
        return InvalidPacket{QStringLiteral("Hashset root does not match file identity")};
    if(size%partSize==0&&body.last(16)!=QCryptographicHash::hash(QByteArrayView{},QCryptographicHash::Md4))
        return InvalidPacket{QStringLiteral("Hashset empty boundary part is invalid")};
    for(int i=0;i<count;++i)hashes.append(p.mid(18+16*i,16));
    return hashes;
}
}

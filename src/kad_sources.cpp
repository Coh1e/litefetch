#include "kad_sources.h"
#include <QtEndian>
#include <QSet>
#include <limits>
#include <optional>
#include <stdexcept>
namespace downloader::kad {
namespace {
QByteArray idBytes(QByteArrayView id){QByteArray result(16,Qt::Uninitialized);
    for(int i=0;i<16;i+=4)for(int j=0;j<4;++j)result[i+j]=id[i+3-j];return result;}
class Reader {
public:
    explicit Reader(QByteArrayView bytes):bytes_(bytes){}
    QByteArrayView take(quint64 count){if(count>quint64(bytes_.size()))throw std::runtime_error("Truncated Kad source page");const auto result=bytes_.first(qsizetype(count));bytes_=bytes_.sliced(qsizetype(count));return result;}
    template<class T>T integer(){return qFromLittleEndian<T>(take(sizeof(T)).data());}
    bool empty()const{return bytes_.isEmpty();}
private:QByteArrayView bytes_;
};
std::optional<quint64> value(Reader& r,quint8 type){
    switch(type){
    case 1:r.take(16);break;
    case 2:{const auto length=r.integer<quint16>();if(length>4096)throw std::runtime_error("Kad source string too large");r.take(length);break;}
    case 3:return r.integer<quint32>();
    case 4:r.take(4);break;
    case 8:return r.integer<quint16>();
    case 9:return r.integer<quint8>();
    case 10:r.take(r.integer<quint8>());break;
    case 11:return r.integer<quint64>();
    default:throw std::runtime_error("Unknown Kad source tag type");
    }
    return {};
}
}
QByteArray sourceRequest(const QByteArray& hash,quint64 size,quint16 start){
    if(hash.size()!=16||!size||start>0x7fff)throw std::invalid_argument("Invalid Kad file source request");
    QByteArray bytes=QByteArray::fromHex("e434")+idBytes(hash);char tail[10];qToLittleEndian(start,tail);qToLittleEndian(size,tail+2);bytes.append(tail,10);return bytes;
}
SourcePageResult parseSourcePage(QByteArrayView bytes,const QByteArray& hash,quint64 size,const QByteArray& responder){
    if(hash.size()!=16||responder.size()!=16||!size||bytes.size()<36||bytes.size()>60000||quint8(bytes[0])!=0xe4||quint8(bytes[1])!=0x3b)
        return InvalidDatagram{QStringLiteral("Invalid Kad source response")};
    try{
        Reader r(bytes.sliced(2));
        if(idBytes(r.take(16))!=responder||idBytes(r.take(16))!=hash)throw std::runtime_error("Kad source response identity mismatch");
        SourcePage result;result.entries=r.integer<quint16>();if(result.entries>50)throw std::runtime_error("Kad source result limit exceeded");
        QSet<quint64> endpoints;QSet<QByteArray> users;
        for(int index=0;index<result.entries;++index){
            FileSource source;source.userHash=idBytes(r.take(16));const auto tags=r.integer<quint8>();
            if(tags>64)throw std::runtime_error("Too many Kad source tags");QSet<quint8> seen;bool matchingSize=true;
            for(int t=0;t<tags;++t){const auto type=r.integer<quint8>();const auto length=r.integer<quint16>();
                if(!length||length>128)throw std::runtime_error("Invalid Kad tag name");
                const auto name=r.take(length);const quint8 id=length==1?quint8(name[0]):quint8(0);const auto number=value(r,type);
                const bool recognized=id==0xff||id==0xfe||id==0xfd||id==0xfc||id==0xf3||id==2;
                if(!recognized)continue;
                if(seen.contains(id)||!number)throw std::runtime_error("Invalid or duplicate Kad source field");seen.insert(id);
                const auto n=*number;
                if(id==0xff||id==0xf3){if(n>255)throw std::runtime_error("Kad source flag overflow");if(id==0xff)source.type=quint8(n);else source.cryptOptions=quint8(n);}
                else if(id==0xfe){if(n>std::numeric_limits<quint32>::max())throw std::runtime_error("Kad source IP overflow");source.ipv4=quint32(n);}
                else if(id==0xfd||id==0xfc){if(n>65535)throw std::runtime_error("Kad source port overflow");if(id==0xfd)source.tcpPort=quint16(n);else source.udpPort=quint16(n);}
                else matchingSize=!n||n==size;
            }
            if(!matchingSize||source.userHash==QByteArray(16,'\0')||!source.ipv4||!source.tcpPort||source.ipv4==0xffffffff||(source.ipv4>>28)==0xe)continue;
            if(source.type!=1&&source.type!=3&&source.type!=4&&source.type!=5&&source.type!=6)continue;
            const auto endpoint=(quint64(source.ipv4)<<16)|source.tcpPort;
            if(users.contains(source.userHash)||endpoints.contains(endpoint))continue;
            users.insert(source.userHash);endpoints.insert(endpoint);result.sources.append(std::move(source));
        }
        if(!r.empty())throw std::runtime_error("Trailing Kad source response bytes");return result;
    }catch(const std::exception& error){return InvalidDatagram{QString::fromLatin1(error.what())};}
}
}

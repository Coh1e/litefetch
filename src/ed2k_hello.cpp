#include "ed2k_hello.h"
#include <QtEndian>
#include <stdexcept>
namespace downloader::ed2k {
namespace {
class Reader {
public:
    explicit Reader(QByteArrayView input):input_(input){}
    qsizetype remaining()const{return input_.size()-position_;}
    QByteArray bytes(quint64 count){
        if(count>quint64(remaining()))throw std::runtime_error("Truncated hello field");
        const auto value=input_.sliced(position_,static_cast<qsizetype>(count));
        position_+=static_cast<qsizetype>(count);return QByteArray(value.data(),value.size());
    }
    template<class T>T integer(){
        if(remaining()<qsizetype(sizeof(T)))throw std::runtime_error("Truncated hello integer");
        const auto value=qFromLittleEndian<T>(input_.data()+position_);position_+=sizeof(T);return value;
    }
private:
    QByteArrayView input_;qsizetype position_=0;
};
template<class T>void integer(QByteArray& output,T value){
    const auto position=output.size();output.resize(position+sizeof(T));qToLittleEndian<T>(value,output.data()+position);
}
HelloTag tag(Reader& reader){
    HelloTag result;
    auto type=reader.integer<quint8>();
    if(type&0x80){result.id=reader.integer<quint8>();type&=0x7f;}
    else{
        const auto length=reader.integer<quint16>();
        if(length>256)throw std::runtime_error("Hello tag name too long");
        if(length==1)result.id=reader.integer<quint8>();else result.name=reader.bytes(length);
    }
    result.type=type;
    switch(type){
    case 0x01:result.value=reader.bytes(16);break;
    case 0x02:{const auto length=reader.integer<quint16>();if(length>8192)throw std::runtime_error("Hello string too long");result.value=reader.bytes(length);break;}
    case 0x03:result.value=quint64(reader.integer<quint32>());break;
    case 0x04:result.value=reader.bytes(4);break;
    case 0x05:case 0x09:result.value=quint64(reader.integer<quint8>());break;
    case 0x06:{const auto bits=reader.integer<quint16>();result.value=reader.bytes(bits/8+1);break;}
    case 0x07:{const auto length=reader.integer<quint32>();if(length>65536)throw std::runtime_error("Hello blob too long");result.value=reader.bytes(length);break;}
    case 0x08:result.value=quint64(reader.integer<quint16>());break;
    case 0x0a:result.value=reader.bytes(reader.integer<quint8>());break;
    case 0x0b:result.value=reader.integer<quint64>();break;
    default:
        if(type>=0x11&&type<=0x20)result.value=reader.bytes(type-0x10);
        else throw std::runtime_error("Unknown hello tag type");
    }
    return result;
}
}
HelloResult parseHello(const Packet& packet){
    if(packet.protocol!=Protocol::Edonkey||(packet.opcode!=0x01&&packet.opcode!=0x4c))
        return InvalidPacket{QStringLiteral("不是ed2k握手报文。")};
    if(packet.payload.size()>65536)return InvalidPacket{QStringLiteral("ed2k握手报文过大。")};
    try{
        Reader reader(packet.payload);
        if(packet.opcode==0x01&&reader.integer<quint8>()!=16)throw std::runtime_error("Invalid hello hash length");
        Hello result;result.userHash=reader.bytes(16);result.clientId=reader.integer<quint32>();
        result.tcpPort=reader.integer<quint16>();
        const auto count=reader.integer<quint32>();if(count>128)throw std::runtime_error("Too many hello tags");
        bool nameSeen=false,versionSeen=false,portSeen=false;
        for(quint32 i=0;i<count;++i){
            auto value=tag(reader);
            if(value.id==1){
                if(nameSeen||!std::holds_alternative<QByteArray>(value.value)
                    ||!(value.type==2||(value.type>=0x11&&value.type<=0x20)))throw std::runtime_error("Invalid hello name tag");
                nameSeen=true;
            }
            if(value.id==0x11||value.id==0x0f){
                auto& seen=value.id==0x11?versionSeen:portSeen;
                if(seen||!std::holds_alternative<quint64>(value.value)
                    ||(value.type!=3&&value.type!=8&&value.type!=9&&value.type!=0x0b))throw std::runtime_error("Invalid hello integer tag");
                seen=true;
                if(value.id==0x0f){const auto port=std::get<quint64>(value.value);if(port>65535)throw std::runtime_error("Invalid hello port");result.tcpPort=static_cast<quint16>(port);}
            }
            result.tags.append(std::move(value));
        }
        result.serverIp=reader.integer<quint32>();result.serverPort=reader.integer<quint16>();
        if(reader.remaining()!=0&&reader.remaining()!=4&&reader.remaining()!=6)throw std::runtime_error("Unexpected hello trailer");
        result.trailer=reader.bytes(reader.remaining());return result;
    }catch(const std::exception& error){return InvalidPacket{QString::fromLatin1(error.what())};}
}
Packet makeHello(const QByteArray& hash,quint32 id,quint16 port,const QString& nickname,bool answer){
    const auto name=nickname.toUtf8();
    if(hash.size()!=16||name.isEmpty()||name.size()>1024||nickname.contains(QChar::Null))throw std::invalid_argument("Invalid local ed2k identity");
    QByteArray payload;if(!answer)payload.append(char(16));payload.append(hash);
    integer(payload,id);integer(payload,port);integer<quint32>(payload,2);
    payload.append(char(2));integer<quint16>(payload,1);payload.append(char(1));integer<quint16>(payload,static_cast<quint16>(name.size()));payload.append(name);
    payload.append(char(3));integer<quint16>(payload,1);payload.append(char(0x11));integer<quint32>(payload,0x3c);
    integer<quint32>(payload,0);integer<quint16>(payload,0);
    return {Protocol::Edonkey,static_cast<quint8>(answer?0x4c:0x01),payload};
}
}

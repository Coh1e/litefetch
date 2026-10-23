#include "ed2k_server.h"
#include <QtEndian>
#include <stdexcept>
namespace downloader::ed2k {
namespace {
int remaining(QDeadlineTimer& timer){const auto n=timer.remainingTime();if(n<=0)throw std::runtime_error("ed2k server deadline expired");return static_cast<int>(n);}
void validateTimeout(int n){if(n<1||n>60000)throw std::invalid_argument("Invalid server timeout");}
bool notification(const Packet& packet){
    if(packet.protocol!=Protocol::Edonkey)return false;
    if(packet.opcode==0x34)return packet.payload.size()==8;
    if(packet.opcode==0x38)return packet.payload.size()>=2&&packet.payload.size()==2+qFromLittleEndian<quint16>(packet.payload.constData());
    // Advisory server identity is bounded by the transport and not used as trusted state.
    if(packet.opcode==0x41)return packet.payload.size()>=26;
    return false;
}
}
ServerIdentity ServerConnection::login(const QHostAddress& address,quint16 port,const QByteArray& hash,
    quint16 incomingPort,const QString& nickname,int timeout){
    validateTimeout(timeout);close();
    // Basic legacy login: hash, prior ID, listening port, name/version tags.
    // Do not advertise compression, cryptography or other unimplemented features.
    auto login=makeHello(hash,0,incomingPort,nickname,true);login.opcode=1;login.payload.chop(6);
    QDeadlineTimer timer(timeout);
    try{
        peer_.connectTransport(address,port,remaining(timer));peer_.send(login,remaining(timer));
        for(int count=0;count<64;++count){const auto packet=peer_.receive(remaining(timer),65536);
            if(packet.protocol==Protocol::Edonkey&&packet.opcode==0x40){
                const auto n=packet.payload.size();
                if(n!=4&&n!=8&&n!=12&&n!=16&&n!=20)throw std::runtime_error("Invalid server login reply");
                identity_={qFromLittleEndian<quint32>(packet.payload.constData()),n>=8?qFromLittleEndian<quint32>(packet.payload.constData()+4):0};
                if(!identity_.clientId)throw std::runtime_error("Server rejected client identity");
                loggedIn_=true;return identity_;
            }
            if(!notification(packet))throw std::runtime_error("Unexpected server login packet");
        }
        throw std::runtime_error("Server login packet budget exceeded");
    }catch(...){close();throw;}
}
QVector<ServerSource> ServerConnection::sources(const QByteArray& hash,quint64 size,int timeout){
    validateTimeout(timeout);if(!loggedIn_)throw std::logic_error("Server is not logged in");
    const auto request=requestSources(hash,size,(identity_.flags&0x100)!=0);QDeadlineTimer timer(timeout);
    try{
        peer_.send(request,remaining(timer));
        for(int count=0;count<64;++count){const auto packet=peer_.receive(remaining(timer),65536);
            if(packet.protocol==Protocol::Edonkey&&packet.opcode==0x42){
                auto result=parseSources(packet,hash);
                if(auto* sources=std::get_if<QVector<ServerSource>>(&result))return std::move(*sources);
                throw std::runtime_error("Invalid server source reply");
            }
            if(!notification(packet))throw std::runtime_error("Unexpected server source packet");
        }
        throw std::runtime_error("Server source packet budget exceeded");
    }catch(...){close();throw;}
}
}

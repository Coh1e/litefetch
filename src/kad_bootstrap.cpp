#include "kad_bootstrap.h"
#include <QtEndian>
#include <QSet>
namespace downloader::kad {
namespace {
QByteArray nodeId(const char* input){QByteArray id(16,Qt::Uninitialized);
    for(int i=0;i<4;++i)qToBigEndian(qFromLittleEndian<quint32>(input+i*4),id.data()+i*4);return id;}
bool zero(const QByteArray& id){return id==QByteArray(16,'\0');}
}
QByteArray bootstrapRequest(){return QByteArray::fromHex("e401");}
BootstrapResult parseBootstrap(QByteArrayView datagram){
    if(datagram.size()<23||datagram.size()>bootstrapDatagramLimit||quint8(datagram[0])!=0xe4||quint8(datagram[1])!=0x09)
        return InvalidDatagram{QStringLiteral("Invalid Kad bootstrap datagram")};
    const auto* bytes=datagram.data();const auto count=qFromLittleEndian<quint16>(bytes+21);
    if(count>20||datagram.size()!=23+25*count)return InvalidDatagram{QStringLiteral("Invalid Kad bootstrap contact count")};
    Bootstrap result;result.id=nodeId(bytes+2);result.tcpPort=qFromLittleEndian<quint16>(bytes+18);result.version=quint8(bytes[20]);
    if(zero(result.id)||!result.tcpPort||result.version<2)return InvalidDatagram{QStringLiteral("Invalid Kad bootstrap identity")};
    QSet<QByteArray> ids;QSet<quint64> endpoints;
    for(int i=0;i<count;++i){const auto* p=bytes+23+i*25;
        Contact contact{nodeId(p),qFromLittleEndian<quint32>(p+16),qFromLittleEndian<quint16>(p+20),qFromLittleEndian<quint16>(p+22),quint8(p[24])};
        const auto endpoint=(quint64(contact.ipv4)<<16)|contact.udpPort;
        if(zero(contact.id)||contact.id==result.id||!contact.ipv4||!contact.udpPort||!contact.tcpPort||contact.version<2||ids.contains(contact.id)||endpoints.contains(endpoint))continue;
        ids.insert(contact.id);endpoints.insert(endpoint);result.contacts.append(std::move(contact));
    }
    return result;
}
}

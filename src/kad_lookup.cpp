#include "kad_lookup.h"
#include <QtEndian>
#include <QSet>
#include <stdexcept>
namespace downloader::kad {
namespace {
QByteArray wireId(QByteArrayView id){QByteArray result(16,Qt::Uninitialized);
    for(int i=0;i<16;i+=4)for(int j=0;j<4;++j)result[i+j]=id[i+3-j];return result;}
}
QByteArray xorDistance(const QByteArray& left,const QByteArray& right){
    if(left.size()!=16||right.size()!=16)throw std::invalid_argument("Invalid Kad distance ID");
    QByteArray result(16,Qt::Uninitialized);for(int i=0;i<16;++i)result[i]=char(quint8(left[i])^quint8(right[i]));return result;
}
QByteArray lookupRequest(const QByteArray& target,const QByteArray& receiverId,quint8 count){
    if(target.size()!=16||receiverId.size()!=16||receiverId==QByteArray(16,'\0')||!count||count>31)throw std::invalid_argument("Invalid Kad lookup request");
    QByteArray bytes=QByteArray::fromHex("e421");bytes.append(char(count));return bytes+wireId(target)+wireId(receiverId);
}
ContactsResult parseLookup(QByteArrayView bytes,const QByteArray& target,quint8 maxContacts){
    if(target.size()!=16||!maxContacts||maxContacts>32||bytes.size()<19||bytes.size()>819||quint8(bytes[0])!=0xe4||quint8(bytes[1])!=0x29)
        return InvalidDatagram{QStringLiteral("Invalid Kad lookup reply")};
    if(wireId(bytes.sliced(2,16))!=target)return InvalidDatagram{QStringLiteral("Kad lookup target mismatch")};
    const auto count=quint8(bytes[18]);if(count>maxContacts||bytes.size()!=19+25*count)return InvalidDatagram{QStringLiteral("Invalid Kad lookup reply count")};
    QVector<Contact> result;QSet<QByteArray> ids;QSet<quint64> endpoints;
    for(int i=0;i<count;++i){const auto* p=bytes.data()+19+25*i;
        Contact contact{wireId(QByteArrayView(p,16)),qFromLittleEndian<quint32>(p+16),qFromLittleEndian<quint16>(p+20),qFromLittleEndian<quint16>(p+22),quint8(p[24])};
        const auto endpoint=(quint64(contact.ipv4)<<16)|contact.udpPort;
        if(contact.id==QByteArray(16,'\0')||!contact.ipv4||!contact.udpPort||!contact.tcpPort||contact.version<2||ids.contains(contact.id)||endpoints.contains(endpoint))continue;
        ids.insert(contact.id);endpoints.insert(endpoint);result.append(std::move(contact));
    }
    return result;
}
}

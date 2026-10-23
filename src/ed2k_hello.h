#pragma once
#include "ed2k_wire.h"
#include <QVector>
namespace downloader::ed2k {
struct HelloTag {
    quint8 type = 0;
    quint8 id = 0;
    QByteArray name;
    std::variant<quint64,QByteArray> value;
};
struct Hello {
    QByteArray userHash;
    quint32 clientId = 0;
    quint16 tcpPort = 0;
    QVector<HelloTag> tags;
    quint32 serverIp = 0;
    quint16 serverPort = 0;
    QByteArray trailer;
};
using HelloResult=std::variant<Hello,InvalidPacket>;
HelloResult parseHello(const Packet& packet);
// Advertises only a basic ed2k identity, with no unimplemented eMule/Kad flags.
Packet makeHello(const QByteArray& userHash,quint32 clientId,quint16 tcpPort,
    const QString& nickname,bool answer=false);
}

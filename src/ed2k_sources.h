#pragma once
#include "ed2k_wire.h"
#include <QVector>
namespace downloader::ed2k {
struct ServerSource {
    quint32 clientId;
    quint16 port;
    bool needsCallback()const{return clientId<0x01000000;}
};
Packet requestSources(const QByteArray& hash,quint64 size,bool largeSupported);
using SourcesResult=std::variant<QVector<ServerSource>,InvalidPacket>;
SourcesResult parseSources(const Packet& packet,const QByteArray& hash);
}

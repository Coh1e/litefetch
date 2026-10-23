#pragma once
#include "ed2k_wire.h"
#include <QVector>
namespace downloader::ed2k {
using HashsetResult=std::variant<QVector<QByteArray>,InvalidPacket>;
// Only returns part hashes after count, identity and aggregate root validation.
HashsetResult parseHashset(const Packet& packet,const QByteArray& fileHash,quint64 size);
}

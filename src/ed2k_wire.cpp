#include "ed2k_wire.h"
#include <QtEndian>
#include <limits>
#include <stdexcept>
namespace downloader::ed2k {
namespace {
bool valid(quint8 protocol){return protocol==0xe3||protocol==0xc5||protocol==0xd4;}
}
DecodeResult decode(QByteArrayView input,quint32 limit)
{
    if(input.empty())return IncompletePacket{};
    const auto protocol=static_cast<quint8>(input[0]);
    if(!valid(protocol))return InvalidPacket{QStringLiteral("未知的ed2k TCP协议标记。")};
    if(input.size()<5)return IncompletePacket{};
    const auto length=qFromLittleEndian<quint32>(input.data()+1);
    if(length==0)return InvalidPacket{QStringLiteral("ed2k数据包缺少操作码。")};
    const auto payloadSize=length-1;
    if(payloadSize>limit)return InvalidPacket{QStringLiteral("ed2k数据包超过长度限制。")};
    const quint64 total=quint64(length)+5;
    if(total>quint64(std::numeric_limits<qsizetype>::max()))return InvalidPacket{QStringLiteral("ed2k数据包长度溢出。")};
    if(quint64(input.size())<total)return IncompletePacket{};
    return DecodedPacket{{static_cast<Protocol>(protocol),static_cast<quint8>(input[5]),
        QByteArray(input.data()+6,payloadSize)},static_cast<qsizetype>(total)};
}
QByteArray encode(const Packet& packet,quint32 limit)
{
    if(!valid(static_cast<quint8>(packet.protocol))||quint64(packet.payload.size())>limit
        ||quint64(packet.payload.size())>=std::numeric_limits<quint32>::max())
        throw std::invalid_argument("Invalid ed2k TCP packet");
    QByteArray bytes(6,Qt::Uninitialized);
    bytes[0]=static_cast<char>(packet.protocol);
    qToLittleEndian<quint32>(static_cast<quint32>(packet.payload.size())+1,bytes.data()+1);
    bytes[5]=static_cast<char>(packet.opcode);bytes.append(packet.payload);return bytes;
}
}

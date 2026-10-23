#pragma once
#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <variant>
namespace downloader::ed2k {
enum class Protocol : quint8 { Edonkey=0xe3, Emule=0xc5, Packed=0xd4 };
struct Packet { Protocol protocol; quint8 opcode; QByteArray payload; };
struct IncompletePacket {};
struct InvalidPacket { QString reason; };
struct DecodedPacket { Packet packet; qsizetype consumed; };
using DecodeResult=std::variant<IncompletePacket,InvalidPacket,DecodedPacket>;
inline constexpr quint32 defaultPayloadLimit=4*1024*1024;
// TCP framing only. Packed bodies remain compressed and require a separate
// bounded decompressor. UDP Kad packets use a different format.
DecodeResult decode(QByteArrayView input,quint32 payloadLimit=defaultPayloadLimit);
QByteArray encode(const Packet& packet,quint32 payloadLimit=defaultPayloadLimit);
}

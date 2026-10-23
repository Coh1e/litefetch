#pragma once
#include <QByteArrayView>
#include <QByteArray>
#include <QString>
#include <QVector>
#include <variant>
#include <functional>
namespace downloader::kad {
using CancelCheck=std::function<bool()>;
struct Contact {
    QByteArray id; // Canonical big-endian 128-bit ID, not the mixed-endian wire bytes.
    quint32 ipv4=0; // Numeric host order, e.g. 0xC0000201 = 192.0.2.1.
    quint16 udpPort=0,tcpPort=0;
    quint8 version=0;
};
struct Bootstrap {QByteArray id;quint16 tcpPort=0;quint8 version=0;QVector<Contact> contacts;};
struct InvalidDatagram {QString reason;};
using BootstrapResult=std::variant<Bootstrap,InvalidDatagram>;
inline constexpr qsizetype bootstrapDatagramLimit=523; // 2 + 19 + 2 + 20*25.
QByteArray bootstrapRequest();
BootstrapResult parseBootstrap(QByteArrayView datagram);
}

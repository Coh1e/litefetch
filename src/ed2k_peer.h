#pragma once
#include "ed2k_hello.h"
#include <QTcpSocket>
#include <QDeadlineTimer>
#include <functional>
namespace downloader::ed2k {
// Blocking connection owned by one worker thread. No GUI objects or child
// processes. Buffers and deadlines are bounded; errors do not auto-reconnect.
class PeerConnection {
public:
    PeerConnection(){socket_.setReadBufferSize(defaultPayloadLimit+6);}
    void setCancellationCheck(std::function<bool()> check){cancelled_=std::move(check);}
    void connectTransport(const QHostAddress& address,quint16 port,int timeoutMs=5000);
    Hello connectHello(const QHostAddress& address,quint16 port,const Packet& greeting,int timeoutMs=5000);
    void send(const Packet& packet,int timeoutMs=5000);
    Packet receive(int timeoutMs=5000,quint32 payloadLimit=defaultPayloadLimit);
    void close();
private:
    QTcpSocket socket_;
    QByteArray buffer_;
    std::function<bool()> cancelled_;
    void checkStop()const;
    void sendUntil(const Packet& packet,QDeadlineTimer& deadline);
    Packet receiveUntil(QDeadlineTimer& deadline,quint32 limit);
};
}

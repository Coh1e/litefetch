#pragma once
#include "ed2k_peer.h"
#include "ed2k_sources.h"
namespace downloader::ed2k {
struct ServerIdentity {quint32 clientId=0,flags=0;};
// Caller supplies a server explicitly. No public server bootstrap or background retries.
class ServerConnection {
public:
    void setCancellationCheck(std::function<bool()> check){peer_.setCancellationCheck(std::move(check));}
    ServerIdentity login(const QHostAddress& address,quint16 port,const QByteArray& userHash,
        quint16 incomingPort,const QString& nickname,int timeoutMs=5000);
    QVector<ServerSource> sources(const QByteArray& hash,quint64 size,int timeoutMs=5000);
    void close(){peer_.close();loggedIn_=false;}
private:
    PeerConnection peer_;
    ServerIdentity identity_;
    bool loggedIn_=false;
};
}

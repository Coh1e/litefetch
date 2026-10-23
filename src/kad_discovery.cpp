#include "kad_client.h"
#include "kad_datagram.h"
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QCryptographicHash>
#include <QSet>
#include <stdexcept>
namespace downloader::kad {
SourceCollection collectSources(const Contact& node,const QByteArray& hash,quint64 size,int timeout,int quiet,int maxPages,const CancelCheck& cancelled){
    const QHostAddress address(node.ipv4);
    if(node.id.size()!=16||node.id==QByteArray(16,'\0')||node.version<3||!node.ipv4||address.isMulticast()||address==QHostAddress::Broadcast||
       !node.udpPort||timeout<1||timeout>60000||quiet<1||quiet>5000||maxPages<1||maxPages>6)
        throw std::invalid_argument("Invalid Kad source collection limits");
    const auto request=sourceRequest(hash,size);
    if(cancelled&&cancelled())throw std::runtime_error("Kad collection cancelled");
    QUdpSocket socket;socket.setReadBufferSize(512*1024);
    if(!socket.bind(QHostAddress(QHostAddress::AnyIPv4),quint16(0))||socket.writeDatagram(request,address,node.udpPort)!=request.size())
        throw std::runtime_error("Cannot start Kad source collection");
    QDeadlineTimer deadline(timeout);QElapsedTimer idle;
    SourceCollection result;QSet<QByteArray> pages,users;QSet<quint64> endpoints;int packets=0;
    for(;;){
        if(cancelled&&cancelled())throw std::runtime_error("Kad collection cancelled");
        if(deadline.hasExpired()){result.limitReached=true;return result;}
        if(idle.isValid()&&idle.elapsed()>=quiet){result.quietEnd=true;return result;}
        if(!socket.hasPendingDatagrams()){
            auto wait=deadline.remainingTime();if(idle.isValid())wait=qMin(wait,qMax<qint64>(1,quiet-idle.elapsed()));
            if(!socket.waitForReadyRead(static_cast<int>(qMin<qint64>(100,wait)))){
                if(socket.error()!=QAbstractSocket::SocketTimeoutError)throw std::runtime_error("Kad source receive failed");
                continue;
            }
        }
        while(socket.hasPendingDatagrams()){
            if(cancelled&&cancelled())throw std::runtime_error("Kad collection cancelled");
            if(++packets>64){result.limitReached=true;return result;}
            if(deadline.hasExpired()){result.limitReached=true;return result;}
            const auto length=socket.pendingDatagramSize();const auto datagram=socket.receiveDatagram(60001);
            if(datagram.senderAddress()!=address||datagram.senderPort()!=node.udpPort)continue;
            const auto wire=datagram.data();
            if(wire.size()>=2&&(quint8(wire[0])==0xe4||quint8(wire[0])==0xe5)&&quint8(wire[1])!=0x3b)continue;
            if(length>60000)throw std::runtime_error("Kad source page too large");
            const auto bytes=unpackDatagram(wire,60000);
            auto parsed=parseSourcePage(bytes,hash,size,node.id);const auto* page=std::get_if<SourcePage>(&parsed);
            if(!page)throw std::runtime_error("Invalid Kad source page during collection");
            const auto fingerprint=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);
            if(pages.contains(fingerprint))continue;
            pages.insert(fingerprint);++result.pages;idle.start();
            for(const auto& source:page->sources){const auto endpoint=(quint64(source.ipv4)<<16)|source.tcpPort;
                if(users.contains(source.userHash)||endpoints.contains(endpoint))continue;
                users.insert(source.userHash);endpoints.insert(endpoint);result.sources.append(source);
            }
            if(result.pages>=maxPages||result.sources.size()>=300){result.limitReached=true;return result;}
        }
    }
}
DiscoveryResult discoverSources(const QByteArray& hash,quint64 size,const QVector<Contact>& seeds,int timeout,int maxNodes,int queryTimeout,const CancelCheck& cancelled){
    if(hash.size()!=16||!size||timeout<2||timeout>60000||maxNodes<1||maxNodes>20||queryTimeout<1||queryTimeout>60000)
        throw std::invalid_argument("Invalid Kad file discovery limits");
    QDeadlineTimer deadline(timeout);DiscoveryResult result;
    const auto lookup=findClosest(hash,seeds,qMax(1,timeout/2),16,qMin(queryTimeout,qMax(1,timeout/2)),cancelled);
    result.nodeQueries=lookup.queries;result.limitReached=lookup.budgetExhausted;
    QSet<QByteArray> users;QSet<quint64> endpoints;
    for(const auto& node:lookup.responsive){
        if(cancelled&&cancelled())throw std::runtime_error("Kad discovery cancelled");
        if(node.version<3)continue;
        if(result.sourceQueries>=maxNodes||deadline.hasExpired()){result.limitReached=true;break;}
        ++result.sourceQueries;
        try{
            const auto collected=collectSources(node,hash,size,static_cast<int>(qMin<qint64>(queryTimeout,qMax<qint64>(1,deadline.remainingTime()))),100,6,cancelled);
            result.limitReached|=collected.limitReached;
            for(const auto& source:collected.sources){const auto endpoint=(quint64(source.ipv4)<<16)|source.tcpPort;
                if(users.contains(source.userHash)||endpoints.contains(endpoint))continue;
                users.insert(source.userHash);endpoints.insert(endpoint);result.sources.append(source);
                if(result.sources.size()>=300){result.limitReached=true;return result;}
            }
        }catch(const std::runtime_error&){if(cancelled&&cancelled())throw;++result.failedQueries;}
    }
    return result;
}
}

#include "kad_client.h"
#include "kad_datagram.h"
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QDeadlineTimer>
#include <stdexcept>
#include <QSet>
#include <algorithm>
#include <cstring>
namespace downloader::kad {
namespace {
QByteArray exchange(const QHostAddress& seed,quint16 port,const QByteArray& request,quint8 replyOpcode,qsizetype limit,int timeout,const CancelCheck& cancelled){
    if(seed.protocol()!=QAbstractSocket::IPv4Protocol||!seed.toIPv4Address()||seed.isMulticast()||seed==QHostAddress::Broadcast||!port||timeout<1||timeout>60000)
        throw std::invalid_argument("Invalid Kad endpoint or timeout");
    QUdpSocket socket;socket.setReadBufferSize(65536);
    if(!socket.bind(QHostAddress(QHostAddress::AnyIPv4),quint16(0)))throw std::runtime_error("Cannot bind Kad bootstrap socket");
    QDeadlineTimer timer(timeout);
    if(cancelled&&cancelled())throw std::runtime_error("Kad query cancelled");
    if(socket.writeDatagram(request,seed,port)!=request.size())throw std::runtime_error("Cannot send Kad bootstrap request");
    for(int received=0;received<64;){
        if(cancelled&&cancelled())throw std::runtime_error("Kad query cancelled");
        if(!socket.hasPendingDatagrams()){
            const auto remaining=timer.remainingTime();
            if(remaining<=0)throw std::runtime_error("Kad query timed out");
            if(!socket.waitForReadyRead(static_cast<int>(qMin<qint64>(100,remaining)))){
                if(socket.error()!=QAbstractSocket::SocketTimeoutError)throw std::runtime_error("Kad query receive failed");
                continue;
            }
        }
        while(socket.hasPendingDatagrams()){
            if(cancelled&&cancelled())throw std::runtime_error("Kad query cancelled");
            if(++received>64)throw std::runtime_error("Kad bootstrap datagram budget exceeded");
            if(timer.hasExpired())throw std::runtime_error("Kad bootstrap timed out");
            const auto size=socket.pendingDatagramSize();
            auto datagram=socket.receiveDatagram(limit+1);
            if(datagram.senderAddress()!=seed||datagram.senderPort()!=port)continue;
            const auto bytes=datagram.data();
            if(bytes.size()>=2&&(quint8(bytes[0])==0xe4||quint8(bytes[0])==0xe5)&&quint8(bytes[1])!=replyOpcode)continue;
            if(size>limit)throw std::runtime_error("Kad reply is too large");
            return unpackDatagram(bytes,limit);
        }
    }
    throw std::runtime_error("Kad bootstrap datagram budget exceeded");
}
}
Bootstrap bootstrap(const QHostAddress& seed,quint16 port,int timeout,const CancelCheck& cancelled){
    const auto bytes=exchange(seed,port,bootstrapRequest(),0x09,bootstrapDatagramLimit,timeout,cancelled);auto parsed=parseBootstrap(bytes);
    if(auto* result=std::get_if<Bootstrap>(&parsed))return std::move(*result);
    throw std::runtime_error("Invalid Kad bootstrap reply");
}
QVector<Contact> lookup(const Contact& receiver,const QByteArray& target,int timeout,const CancelCheck& cancelled){
    const auto request=lookupRequest(target,receiver.id);
    const auto bytes=exchange(QHostAddress(receiver.ipv4),receiver.udpPort,request,0x29,819,timeout,cancelled);
    auto parsed=parseLookup(bytes,target);
    if(auto* contacts=std::get_if<QVector<Contact>>(&parsed))return std::move(*contacts);
    throw std::runtime_error("Invalid Kad lookup reply");
}
SourcePage querySourcesPage(const Contact& receiver,const QByteArray& hash,quint64 size,quint16 start,int timeout,const CancelCheck& cancelled){
    if(receiver.id.size()!=16||receiver.id==QByteArray(16,'\0')||receiver.version<3)throw std::invalid_argument("Invalid Kad source-query node");
    const auto request=sourceRequest(hash,size,start);
    const auto bytes=exchange(QHostAddress(receiver.ipv4),receiver.udpPort,request,0x3b,60000,timeout,cancelled);
    auto parsed=parseSourcePage(bytes,hash,size,receiver.id);
    if(auto* page=std::get_if<SourcePage>(&parsed))return std::move(*page);
    throw std::runtime_error("Invalid Kad source reply");
}
LookupResult findClosest(const QByteArray& target,const QVector<Contact>& seeds,int totalTimeout,int maxQueries,int queryTimeout,const CancelCheck& cancelled){
    if(target.size()!=16||seeds.size()>256||totalTimeout<1||totalTimeout>60000||maxQueries<1||maxQueries>64||queryTimeout<1||queryTimeout>60000)
        throw std::invalid_argument("Invalid Kad iterative lookup limits");
    const auto closer=[&](const Contact& a,const Contact& b){const auto left=xorDistance(a.id,target),right=xorDistance(b.id,target);return std::memcmp(left.constData(),right.constData(),16)<0;};
    QVector<Contact> candidates;QSet<QByteArray> triedIds;QSet<quint64> triedEndpoints;
    const auto endpoint=[](const Contact& c){return (quint64(c.ipv4)<<16)|c.udpPort;};
    const auto add=[&](const Contact& c){
        if(c.id.size()!=16||c.id==QByteArray(16,'\0')||!c.ipv4||!c.udpPort||!c.tcpPort||c.version<2||QHostAddress(c.ipv4).isMulticast()||c.ipv4==0xffffffff)return;
        if(triedIds.contains(c.id)||triedEndpoints.contains(endpoint(c)))return;
        for(const auto& known:candidates)if(known.id==c.id||endpoint(known)==endpoint(c))return;
        if(candidates.size()==256){if(!closer(c,candidates.last()))return;candidates.removeLast();}
        candidates.append(c);std::sort(candidates.begin(),candidates.end(),closer);
    };
    for(const auto& seed:seeds)add(seed);
    QDeadlineTimer deadline(totalTimeout);LookupResult result;
    while(result.queries<maxQueries&&!deadline.hasExpired()){
        if(cancelled&&cancelled())throw std::runtime_error("Kad lookup cancelled");
        auto next=std::find_if(candidates.begin(),candidates.end(),[&](const Contact& c){return !triedIds.contains(c.id)&&!triedEndpoints.contains(endpoint(c));});
        if(next==candidates.end())break;
        const auto contact=*next;triedIds.insert(contact.id);triedEndpoints.insert(endpoint(contact));++result.queries;
        const auto remaining=deadline.remainingTime();if(remaining<=0)break;
        try{const auto discovered=lookup(contact,target,static_cast<int>(qMin<qint64>(queryTimeout,remaining)),cancelled);
            result.responsive.append(contact);for(const auto& candidate:discovered)add(candidate);
        }catch(const std::runtime_error&){if(cancelled&&cancelled())throw;/* Try the next candidate within the same total budget. */}
    }
    result.budgetExhausted=std::any_of(candidates.begin(),candidates.end(),[&](const Contact& c){return !triedIds.contains(c.id)&&!triedEndpoints.contains(endpoint(c));})&&
        (result.queries>=maxQueries||deadline.hasExpired());
    std::sort(result.responsive.begin(),result.responsive.end(),closer);if(result.responsive.size()>20)result.responsive.resize(20);return result;
}
}

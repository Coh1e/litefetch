#include "ed2k_peer.h"
#include "socket_connect_wait.h"
#include <stdexcept>
namespace downloader::ed2k {
namespace {
int remaining(QDeadlineTimer& deadline){
    const auto time=deadline.remainingTime();if(time<=0)throw std::runtime_error("ed2k peer deadline expired");
    return static_cast<int>(qMin<qint64>(time,60000));
}
void validateTimeout(int timeout){if(timeout<1||timeout>60000)throw std::invalid_argument("Invalid ed2k peer timeout");}
}
void PeerConnection::close(){socket_.abort();buffer_.clear();}
void PeerConnection::checkStop()const{if(cancelled_&&cancelled_())throw std::runtime_error("ed2k peer cancelled");}
void PeerConnection::connectTransport(const QHostAddress& address,quint16 port,int timeout){
    validateTimeout(timeout);close();QDeadlineTimer deadline(timeout);
    try{checkStop();network::connectToHost(socket_,address,port,deadline,cancelled_);checkStop();}
    catch(...){close();throw;}
}
void PeerConnection::sendUntil(const Packet& packet,QDeadlineTimer& deadline){
    checkStop();
    const auto bytes=encode(packet);
    if(socket_.state()!=QAbstractSocket::ConnectedState||socket_.write(bytes)!=bytes.size())throw std::runtime_error("Cannot send ed2k packet");
    while(socket_.bytesToWrite()>0){checkStop();
        if(!socket_.waitForBytesWritten(qMin(100,remaining(deadline)))&&socket_.error()!=QAbstractSocket::SocketTimeoutError)throw std::runtime_error("ed2k packet send failed");
    }
}
Packet PeerConnection::receiveUntil(QDeadlineTimer& deadline,quint32 limit){
    if(limit>defaultPayloadLimit)throw std::invalid_argument("ed2k receive limit exceeds connection budget");
    for(;;){
        checkStop();
        auto decoded=decode(buffer_,limit);
        if(auto* packet=std::get_if<DecodedPacket>(&decoded)){
            auto result=std::move(packet->packet);buffer_.remove(0,packet->consumed);return result;
        }
        if(std::holds_alternative<InvalidPacket>(decoded))throw std::runtime_error("Invalid ed2k peer frame");
        if(!socket_.bytesAvailable()&&!socket_.waitForReadyRead(qMin(100,remaining(deadline)))){
            if(socket_.error()==QAbstractSocket::SocketTimeoutError)continue;
            throw std::runtime_error("ed2k peer read failed");
        }
        const auto room=qsizetype(limit)+6-buffer_.size();
        if(room<=0)throw std::runtime_error("ed2k peer receive budget exceeded");
        const auto bytes=socket_.read(qMin<qint64>(room,socket_.bytesAvailable()));
        if(bytes.isEmpty())throw std::runtime_error("ed2k peer closed an incomplete packet");
        buffer_.append(bytes);
    }
}
void PeerConnection::send(const Packet& packet,int timeout){validateTimeout(timeout);QDeadlineTimer deadline(timeout);try{sendUntil(packet,deadline);}catch(...){close();throw;}}
Packet PeerConnection::receive(int timeout,quint32 limit){validateTimeout(timeout);QDeadlineTimer deadline(timeout);try{return receiveUntil(deadline,limit);}catch(...){close();throw;}}
Hello PeerConnection::connectHello(const QHostAddress& address,quint16 port,const Packet& greeting,int timeout){
    validateTimeout(timeout);
    if(greeting.opcode!=1||std::holds_alternative<InvalidPacket>(parseHello(greeting)))throw std::invalid_argument("Invalid local ed2k greeting");
    close();QDeadlineTimer deadline(timeout);
    try{
        checkStop();
        network::connectToHost(socket_,address,port,deadline,cancelled_);
        checkStop();
        sendUntil(greeting,deadline);
        const auto answer=receiveUntil(deadline,65536);
        if(answer.opcode!=0x4c)throw std::runtime_error("Unexpected ed2k handshake response");
        auto parsed=parseHello(answer);
        if(auto* hello=std::get_if<Hello>(&parsed))return std::move(*hello);
        throw std::runtime_error("Invalid ed2k peer hello answer");
    }catch(...){close();throw;}
}
}

#include "ed2k_peer.h"
#include <QCoreApplication>
#include <QTcpServer>
#include <QThread>
#include <QtEndian>
#include <future>
#include <thread>
#include <iostream>
#include <memory>
using namespace downloader::ed2k;
static void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static QByteArray frame(quint8 opcode,QByteArray payload){
    QByteArray result(6,Qt::Uninitialized);result[0]=char(0xe3);
    qToLittleEndian<quint32>(static_cast<quint32>(payload.size()+1),result.data()+1);result[5]=char(opcode);return result+payload;
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    const auto hash=QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");
    const auto body=QByteArray::fromHex("10000102030405060708090a0b0c0d0e0f0403020136120200000002010001010041030100113c000000000000000000");
    const auto greeting=makeHello(hash,0x01020304,4662,"A");
    for(int scenario=0;scenario<4;++scenario){
        std::promise<quint16> ready;auto port=ready.get_future();
        std::promise<void> finished;auto result=finished.get_future();
        std::jthread server([&,scenario]{bool published=false;try{
            QTcpServer listener;check(listener.listen(QHostAddress::LocalHost,0),"Fixture listen failed");
            ready.set_value(listener.serverPort());published=true;
            check(listener.waitForNewConnection(3000),"Fixture connection missing");
            const std::unique_ptr<QTcpSocket> socket(listener.nextPendingConnection());
            const auto expected=frame(1,body);QByteArray received;
            while(received.size()<expected.size()){
                if(!socket->bytesAvailable())check(socket->waitForReadyRead(3000),"Fixture greeting read failed");
                received+=socket->read(expected.size()-received.size());
            }
            check(received==expected,"Client emitted unexpected hello wire bytes");
            if(scenario==0){
                QThread::msleep(160); // Cross a poll slice without expiring the overall deadline.
                const auto answer=frame(0x4c,body.mid(1));
                for(const auto byte:answer){socket->write(&byte,1);socket->flush();QThread::msleep(1);}
                socket->write(frame(0x55,"first")+frame(0x56,"second"));
                socket->flush();
            }else if(scenario==1){socket->write(QByteArray::fromHex("e3ffffffff"));socket->flush();}
            else if(scenario==2){socket->write(QByteArray::fromHex("e3100000004c00"));socket->flush();}
            else{QThread::msleep(250);}
            socket->disconnectFromHost();if(socket->state()!=QAbstractSocket::UnconnectedState)socket->waitForDisconnected(1000);
            finished.set_value();
        }catch(...){if(!published)ready.set_exception(std::current_exception());finished.set_exception(std::current_exception());}});
        PeerConnection peer;bool rejected=false,connected=false;
        try{
            const auto hello=peer.connectHello(QHostAddress::LocalHost,port.get(),greeting,scenario==3?100:3000);
            connected=true;
            check(hello.userHash==hash&&hello.tcpPort==4662,"Peer identity mismatch");
            const auto first=peer.receive(1000),second=peer.receive(1000);
            check(first.opcode==0x55&&first.payload=="first"&&second.opcode==0x56&&second.payload=="second","Coalesced packets lost");
        }catch(const std::runtime_error&){if(scenario==0)throw;rejected=true;}
        check(scenario==0||(!connected&&rejected),"Invalid peer was not rejected");peer.close();result.get();
    }
    std::cout<<"ed2k same-process TCP hello: actual wire, fragmented reply, coalesced packets, malformed/partial/timeout checks passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}

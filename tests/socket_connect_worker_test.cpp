#include "ed2k_peer.h"
#include <QCoreApplication>
#include <QTcpServer>
#include <future>
#include <thread>
#include <iostream>
using namespace downloader::ed2k;
static void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    std::promise<quint16> ready;auto port=ready.get_future();std::promise<void> received;auto serverResult=received.get_future();
    std::jthread server([&]{bool announced=false;try{
        QTcpServer listener;check(listener.listen(QHostAddress::LocalHost,0),"Worker fixture listen failed");ready.set_value(listener.serverPort());announced=true;
        check(listener.waitForNewConnection(2000),"Worker fixture accept failed");std::unique_ptr<QTcpSocket> socket(listener.nextPendingConnection());
        const auto expected=QByteArray::fromHex("e30700000055")+"worker";QByteArray bytes;
        while(bytes.size()<expected.size()){
            if(!socket->bytesAvailable())check(socket->waitForReadyRead(2000),"Worker fixture received no frame");bytes+=socket->read(expected.size()-bytes.size());
        }
        check(bytes==expected,"Worker fixture frame differs");received.set_value();
    }catch(...){if(!announced)ready.set_exception(std::current_exception());received.set_exception(std::current_exception());}});
    const auto endpoint=port.get();std::promise<void> sent;auto clientResult=sent.get_future();
    std::jthread client([&]{try{
        PeerConnection peer;peer.connectTransport(QHostAddress::LocalHost,endpoint,1000);peer.send({Protocol::Edonkey,0x55,"worker"},1000);peer.close();sent.set_value();
    }catch(...){sent.set_exception(std::current_exception());}});
    clientResult.get();serverResult.get();std::cout<<"Real TCP connect/send from a fresh std::jthread passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

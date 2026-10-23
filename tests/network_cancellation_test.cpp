#include "kad_download.h"
#include "ed2k_peer.h"
#include <QCoreApplication>
#include <QUdpSocket>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <future>
#include <thread>
#include <atomic>
#include <iostream>
using namespace downloader;
static void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    QTemporaryDir dir(QDir::currentPath()+"/cancel-network-XXXXXX");check(dir.isValid(),"Test directory failed");
    const QByteArray id(16,'n'),hash(16,'h');
    for(int scenario=0;scenario<6;++scenario){
        std::promise<quint16> ready;auto port=ready.get_future();std::promise<void> requestSeen;auto seen=requestSeen.get_future();
        std::promise<void> finished;auto done=finished.get_future();std::atomic_bool cancelled=false;
        std::jthread fixture([&,scenario]{bool announced=false;try{
            if(scenario==5){
                QTcpServer server;check(server.listen(QHostAddress::LocalHost,0),"TCP fixture listen failed");ready.set_value(server.serverPort());announced=true;
                check(server.waitForNewConnection(3000),"TCP cancellation connection missing");std::unique_ptr<QTcpSocket> socket(server.nextPendingConnection());
                if(!socket->bytesAvailable())check(socket->waitForReadyRead(3000),"TCP hello not received");check(!socket->readAll().isEmpty(),"TCP hello empty");
                requestSeen.set_value();socket->waitForDisconnected(2000);
            }else{
                QUdpSocket server;check(server.bind(QHostAddress(QHostAddress::LocalHost),quint16(0)),"UDP fixture bind failed");ready.set_value(server.localPort());announced=true;
                check(server.waitForReadyRead(3000),"UDP cancellation query missing");char bytes[100];check(server.readDatagram(bytes,sizeof(bytes))>=2,"UDP query empty");requestSeen.set_value();
            }
            finished.set_value();
        }catch(...){if(!announced)ready.set_exception(std::current_exception());finished.set_exception(std::current_exception());}});
        std::jthread cancel([&]{seen.wait_for(std::chrono::seconds(3));QThread::msleep(50);cancelled.store(true);});
        const auto endpoint=port.get();const kad::CancelCheck stopped=[&]{return cancelled.load();};
        const kad::Contact node{id,0x7f000001,endpoint,4662,9};bool rejected=false;QElapsedTimer timer;timer.start();
        try{
            if(scenario==0)kad::bootstrap(QHostAddress::LocalHost,endpoint,30000,stopped);
            if(scenario==1)kad::findClosest(hash,{node},30000,32,30000,stopped);
            if(scenario==2)kad::collectSources(node,hash,123,30000,100,6,stopped);
            if(scenario==3)kad::discoverSources(hash,123,{node},30000,4,30000,stopped);
            if(scenario==4){ed2k::ServerDownload request;request.userHash=id;request.fileHash=hash;request.size=123;request.nickname="Fixture";
                request.destination=dir.filePath("cancelled.bin");request.cancelled=stopped;
                kad::DownloadOptions options;options.bootstrapSeeds={{QHostAddress::LocalHost,endpoint}};options.discoveryTimeoutMs=30000;kad::downloadFile(request,options);
            }
            if(scenario==5){ed2k::PeerConnection peer;peer.setCancellationCheck(stopped);peer.connectHello(QHostAddress::LocalHost,endpoint,ed2k::makeHello(id,0,4662,"Fixture"),30000);}
        }catch(const std::runtime_error&){rejected=true;}
        const auto elapsed=timer.elapsed();done.get();check(rejected&&cancelled.load()&&elapsed<1500,"Long network wait did not cancel promptly");
    }
    std::cout<<"30-second TCP/Kad waits cancel within 1.5 seconds\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

#include "kad_client.h"
#include <QCoreApplication>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QThread>
#include <future>
#include <thread>
#include <iostream>
using namespace downloader::kad;
static void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    const auto sender=QByteArray::fromHex("000102030405060708090a0b0c0d0e0f"),hash=QByteArray::fromHex("101112131415161718191a1b1c1d1e1f");
    const auto page=QByteArray::fromHex("e43b03020100070605040b0a09080f0e0d0c13121110171615141b1a19181f1e1d1c010023222120272625242b2a29282f2e2d2c05090100ff01030100fe010200c0080100fd3612080100fc4012090100f300");
    auto other=page;other[36]=char(0x33);other[other.indexOf(QByteArray::fromHex("080100fd3612"))+4]=char(0x37);
    const auto request=QByteArray::fromHex("e43413121110171615141b1a19181f1e1d1c00007b00000000000000");
    for(int scenario=0;scenario<5;++scenario){
        std::promise<quint16> ready;auto port=ready.get_future();std::promise<void> finished;auto done=finished.get_future();
        std::jthread fixture([&,scenario]{bool announced=false;try{
            QUdpSocket socket;check(socket.bind(QHostAddress(QHostAddress::LocalHost),quint16(0)),"Discovery UDP bind failed");ready.set_value(socket.localPort());announced=true;
            check(socket.waitForReadyRead(3000),"Discovery query missing");auto incoming=socket.receiveDatagram(100);
            if(scenario>=3){
                check(incoming.data()==QByteArray::fromHex("e4211413121110171615141b1a19181f1e1d1c03020100070605040b0a09080f0e0d0c"),"Node lookup not performed before source query");
                const auto empty=QByteArray::fromHex("e42913121110171615141b1a19181f1e1d1c00");
                check(socket.writeDatagram(empty,incoming.senderAddress(),incoming.senderPort())==empty.size(),"Lookup reply failed");
                check(socket.waitForReadyRead(3000),"Source query did not follow node lookup");incoming=socket.receiveDatagram(100);
            }
            check(incoming.data()==request,"Collection source query wrong");
            if(scenario==2){QThread::msleep(100);}
            else if(scenario==4){auto bad=page;bad[18]^=1;socket.writeDatagram(bad,incoming.senderAddress(),incoming.senderPort());}
            else{
                if(scenario==3)socket.writeDatagram(QByteArray::fromHex("e560010203"),incoming.senderAddress(),incoming.senderPort());
                const auto first=scenario==1?page:scenario==3?QByteArray::fromHex("e53b")+qCompress(other.mid(2)).mid(4):other;
                check(socket.writeDatagram(first,incoming.senderAddress(),incoming.senderPort())==first.size(),"First page failed");
                if(scenario!=1){QThread::msleep(20);
                    check(socket.writeDatagram(page,incoming.senderAddress(),incoming.senderPort())==page.size(),"Second page failed");
                    check(socket.writeDatagram(other,incoming.senderAddress(),incoming.senderPort())==other.size(),"Duplicate page failed");
                }
            }
            finished.set_value();
        }catch(...){if(!announced)ready.set_exception(std::current_exception());finished.set_exception(std::current_exception());}});
        const Contact node{sender,0x7f000001,port.get(),4662,9};
        if(scenario>=3){const auto result=discoverSources(hash,123,{node},3000,4,1000);
            check(result.nodeQueries==1&&result.sourceQueries==1,"Discovery query accounting wrong");
            if(scenario==3)check(result.sources.size()==2&&result.failedQueries==0&&!result.limitReached,"Lookup and multi-page discovery failed");
            else check(result.sources.isEmpty()&&result.failedQueries==1,"Invalid source page entered discovery result");
        }else{
            const auto result=collectSources(node,hash,123,scenario==2?50:1000,100,scenario==1?1:6);
            if(scenario==0)check(result.sources.size()==2&&result.pages==2&&result.quietEnd&&!result.limitReached,"Reordered/duplicate pages not collected");
            if(scenario==1)check(result.sources.size()==1&&result.pages==1&&result.limitReached&&!result.quietEnd,"Page budget ignored");
            if(scenario==2)check(result.sources.isEmpty()&&result.pages==0&&result.limitReached&&!result.quietEnd,"Silent query reported complete");
        }
        done.get();
    }
    std::cout<<"Kad multi-page collection and discovery pipeline tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

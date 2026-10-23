#include "kad_client.h"
#include <QCoreApplication>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <future>
#include <thread>
#include <iostream>
using namespace downloader::kad;
static void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    const auto sender=QByteArray::fromHex("000102030405060708090a0b0c0d0e0f"),hash=QByteArray::fromHex("101112131415161718191a1b1c1d1e1f");
    const auto page=QByteArray::fromHex("e43b03020100070605040b0a09080f0e0d0c13121110171615141b1a19181f1e1d1c010023222120272625242b2a29282f2e2d2c05090100ff01030100fe010200c0080100fd3612080100fc4012090100f300");
    const auto request=QByteArray::fromHex("e43413121110171615141b1a19181f1e1d1c00007b00000000000000");
    check(sourceRequest(hash,123)==request,"Source query bytes wrong");
    const auto parsed=std::get<SourcePage>(parseSourcePage(page,hash,123,sender));
    check(parsed.entries==1&&parsed.sources.size()==1,"Known source page rejected");
    const auto& source=parsed.sources[0];check(source.userHash.toHex()=="202122232425262728292a2b2c2d2e2f"&&source.ipv4==0xc0000201&&source.tcpPort==4662&&source.udpPort==4672&&source.directlyUsable(),"Source identity/IP/port endian wrong");
    for(qsizetype n=0;n<page.size();++n)check(std::holds_alternative<InvalidDatagram>(parseSourcePage(QByteArrayView(page).first(n),hash,123,sender)),"Truncated source page accepted");
    check(std::holds_alternative<InvalidDatagram>(parseSourcePage(page+"x",hash,123,sender)),"Trailing page bytes accepted");
    check(std::holds_alternative<InvalidDatagram>(parseSourcePage(page,sender,123,sender)),"Wrong file accepted");
    check(std::holds_alternative<InvalidDatagram>(parseSourcePage(page,hash,123,hash)),"Wrong node accepted");
    auto callback=page;callback[callback.indexOf(QByteArray::fromHex("090100ff01"))+4]=3;
    const auto callbackSource=std::get<SourcePage>(parseSourcePage(callback,hash,123,sender)).sources[0];
    check(callbackSource.needsCallback()&&!callbackSource.directlyUsable(),"Callback source treated as direct");
    auto encrypted=page;encrypted[encrypted.size()-1]=4;check(!std::get<SourcePage>(parseSourcePage(encrypted,hash,123,sender)).sources[0].directlyUsable(),"Required encryption ignored");
    auto duplicate=page;duplicate[52]=6;duplicate+=QByteArray::fromHex("080100fd3612");check(std::holds_alternative<InvalidDatagram>(parseSourcePage(duplicate,hash,123,sender)),"Duplicate source port accepted");
    auto overflow=page;overflow.replace(QByteArray::fromHex("080100fd3612"),QByteArray::fromHex("030100fd00000100"));
    check(std::holds_alternative<InvalidDatagram>(parseSourcePage(overflow,hash,123,sender)),"Overflow source port truncated");
    auto badSize=page;badSize[52]=6;badSize+=QByteArray::fromHex("030100027c000000");
    check(std::get<SourcePage>(parseSourcePage(badSize,hash,123,sender)).sources.isEmpty(),"Mismatched file size retained");
    for(int scenario=0;scenario<3;++scenario){
        std::promise<quint16> ready;auto port=ready.get_future();std::promise<void> finished;auto done=finished.get_future();
        std::jthread fixture([&,scenario]{bool announced=false;try{
            QUdpSocket socket;check(socket.bind(QHostAddress(QHostAddress::LocalHost),quint16(0)),"Source UDP fixture bind failed");ready.set_value(socket.localPort());announced=true;
            check(socket.waitForReadyRead(3000),"Source query missing");const auto incoming=socket.receiveDatagram(100);check(incoming.data()==request,"Actual source UDP query differs");
            auto reply=page;if(scenario==1)reply[2]^=1;if(scenario==2)reply[18]^=1;
            if(scenario==0)reply=QByteArray::fromHex("e53b")+qCompress(reply.mid(2)).mid(4);
            check(socket.writeDatagram(reply,incoming.senderAddress(),incoming.senderPort())==reply.size(),"Source UDP response failed");finished.set_value();
        }catch(...){if(!announced)ready.set_exception(std::current_exception());finished.set_exception(std::current_exception());}});
        bool rejected=false;
        try{const auto result=querySourcesPage({sender,0x7f000001,port.get(),4662,9},hash,123,0,3000);check(result.sources.size()==1&&result.sources[0].directlyUsable(),"Actual source response differs");}
        catch(const std::runtime_error&){if(scenario==0)throw;rejected=true;}
        done.get();check(scenario==0||rejected,"Mismatched source query response accepted");
    }
    std::cout<<"Kad file source page and UDP query tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

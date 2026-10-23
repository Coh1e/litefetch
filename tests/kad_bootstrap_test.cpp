#include "kad_client.h"
#include "kad_datagram.h"
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
    const auto prefix=QByteArray::fromHex("e40903020100070605040b0a09080f0e0d0c361209");
    const auto contact=QByteArray::fromHex("13121110171615141b1a19181f1e1d1c010200c04012361209");
    const auto reply=prefix+QByteArray::fromHex("0100")+contact;
    check(bootstrapRequest()==QByteArray::fromHex("e401"),"Kad request used TCP framing");
    const auto parsed=std::get<Bootstrap>(parseBootstrap(reply));
    check(parsed.id.toHex()=="000102030405060708090a0b0c0d0e0f"&&parsed.tcpPort==4662&&parsed.version==9,"Kad identity endian mismatch");
    check(parsed.contacts.size()==1&&parsed.contacts[0].id.toHex()=="101112131415161718191a1b1c1d1e1f"&&
        parsed.contacts[0].ipv4==0xc0000201&&parsed.contacts[0].udpPort==4672&&parsed.contacts[0].tcpPort==4662,"Kad contact endian mismatch");
    for(qsizetype length=0;length<reply.size();++length)check(std::holds_alternative<InvalidDatagram>(parseBootstrap(QByteArrayView(reply).first(length))),"Truncated bootstrap accepted");
    check(std::holds_alternative<InvalidDatagram>(parseBootstrap(reply+"x")),"Trailing Kad bytes accepted");
    check(std::get<Bootstrap>(parseBootstrap(prefix+QByteArray::fromHex("0200")+contact+contact)).contacts.size()==1,"Kad contacts not deduplicated");
    check(std::get<Bootstrap>(parseBootstrap(prefix+QByteArray::fromHex("0000"))).contacts.isEmpty(),"Empty bootstrap rejected");
    auto packed=reply;packed[0]=char(0xe5);check(std::holds_alternative<InvalidDatagram>(parseBootstrap(packed)),"Packed data treated as uncompressed");
    const auto compressed=QByteArray::fromHex("e509")+qCompress(reply.mid(2)).mid(4);
    check(unpackDatagram(compressed,523)==reply,"Compressed frame changed payload");
    check(unpackDatagram(reply,523)==reply,"Plain frame changed payload");
    const auto rejects=[](const QByteArray& bytes,qsizetype limit){try{unpackDatagram(bytes,limit);return false;}catch(const std::runtime_error&){return true;}};
    for(qsizetype n=0;n<compressed.size();++n)check(rejects(compressed.first(n),523),"Truncated zlib stream accepted");
    check(rejects(compressed+"x",523),"Trailing compressed data accepted");
    auto corrupt=compressed;corrupt[corrupt.size()-1]^=1;check(rejects(corrupt,523),"Corrupt zlib checksum accepted");
    check(rejects(QByteArray::fromHex("e509")+qCompress(QByteArray(60000,'x')).mid(4),523),"Expansion beyond bootstrap limit accepted");
    const auto repeated=QByteArray::fromHex("e409")+QByteArray(500,'x');
    const auto compact=QByteArray::fromHex("e509")+qCompress(repeated.mid(2)).mid(4);
    check(unpackDatagram(compact,repeated.size())==repeated,"Exact expansion limit rejected");
    check(rejects(compact,repeated.size()-1),"Expansion limit off by one");
    auto excessive=reply;excessive[21]=21;check(std::holds_alternative<InvalidDatagram>(parseBootstrap(excessive)),"Excessive contact count accepted");
    for(int scenario=0;scenario<7;++scenario){
        std::promise<quint16> ready;auto port=ready.get_future();std::promise<void> finished;auto done=finished.get_future();
        std::jthread fixture([&,scenario]{bool announced=false;try{
            QUdpSocket socket;check(socket.bind(QHostAddress(QHostAddress::LocalHost),quint16(0)),"UDP fixture bind failed");
            ready.set_value(socket.localPort());announced=true;
            check(socket.waitForReadyRead(3000),"UDP request missing");const auto request=socket.receiveDatagram(100);
            check(request.data()==QByteArray::fromHex("e401"),"Unexpected actual UDP request bytes");
            if(scenario==0){
                QUdpSocket other;auto spoof=reply;spoof[2]=char(0xff);
                check(other.writeDatagram(spoof,request.senderAddress(),request.senderPort())==spoof.size(),"Spoof fixture send failed");
                check(socket.writeDatagram(reply,request.senderAddress(),request.senderPort())==reply.size(),"UDP response send failed");
            }else if(scenario==1){const auto bad=reply.first(22);socket.writeDatagram(bad,request.senderAddress(),request.senderPort());}
            else if(scenario==2){QThread::msleep(150);}
            else if(scenario==3){socket.writeDatagram(QByteArray(600,'x'),request.senderAddress(),request.senderPort());}
            else if(scenario==4){const auto compressed=QByteArray::fromHex("e509")+qCompress(reply.mid(2)).mid(4);socket.writeDatagram(compressed,request.senderAddress(),request.senderPort());}
            else if(scenario==5){
                socket.writeDatagram(QByteArray::fromHex("e460"),request.senderAddress(),request.senderPort());
                socket.writeDatagram(reply,request.senderAddress(),request.senderPort());
            }
            else{for(int i=0;i<65;++i)socket.writeDatagram(QByteArray::fromHex("e460"),request.senderAddress(),request.senderPort());}
            finished.set_value();
        }catch(...){if(!announced)ready.set_exception(std::current_exception());finished.set_exception(std::current_exception());}});
        bool rejected=false;
        try{const auto result=bootstrap(QHostAddress::LocalHost,port.get(),scenario==2?50:3000);
            check(result.id==parsed.id&&result.contacts.size()==1,"Wrong-endpoint datagram accepted");
        }catch(const std::runtime_error&){if(scenario==0||scenario==4||scenario==5)throw;rejected=true;}
        done.get();check(scenario==0||scenario==4||scenario==5||rejected,"Invalid UDP scenario succeeded");
    }
    std::cout<<"Kad bootstrap wire and actual UDP tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

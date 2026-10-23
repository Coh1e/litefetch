#include "kad_client.h"
#include <QCoreApplication>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QtEndian>
#include <future>
#include <thread>
#include <iostream>
using namespace downloader::kad;
static void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
static QByteArray wire(const QByteArray& id){QByteArray bytes(16,Qt::Uninitialized);for(int i=0;i<16;i+=4)qToLittleEndian(qFromBigEndian<quint32>(id.constData()+i),bytes.data()+i);return bytes;}
static QByteArray contactBytes(const Contact& c){auto bytes=wire(c.id);char raw[9];qToLittleEndian(c.ipv4,raw);qToLittleEndian(c.udpPort,raw+4);qToLittleEndian(c.tcpPort,raw+6);raw[8]=char(c.version);bytes.append(raw,9);return bytes;}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    const auto target=QByteArray::fromHex("000102030405060708090a0b0c0d0e0f"),id=QByteArray::fromHex("101112131415161718191a1b1c1d1e1f");
    check(lookupRequest(target,id)==QByteArray::fromHex("e4211403020100070605040b0a09080f0e0d0c13121110171615141b1a19181f1e1d1c"),"Lookup request mixed endian mismatch");
    check(xorDistance(target,id)==QByteArray(16,char(0x10)),"XOR distance incorrect");
    const auto response=QByteArray::fromHex("e42903020100070605040b0a09080f0e0d0c0113121110171615141b1a19181f1e1d1c010200c04012361209");
    const auto contacts=std::get<QVector<Contact>>(parseLookup(response,target));check(contacts.size()==1&&contacts[0].id==id&&contacts[0].ipv4==0xc0000201,"Lookup reply parse mismatch");
    for(qsizetype size=0;size<response.size();++size)check(std::holds_alternative<InvalidDatagram>(parseLookup(QByteArrayView(response).first(size),target)),"Truncated lookup accepted");
    check(std::holds_alternative<InvalidDatagram>(parseLookup(response,id)),"Wrong target accepted");
    check(std::holds_alternative<InvalidDatagram>(parseLookup(response+"x",target)),"Trailing lookup data accepted");
    const QByteArray destination(16,'\0');
    for(int scenario=0;scenario<3;++scenario){
        std::promise<Contact> ready;auto first=ready.get_future();std::promise<void> finished;auto done=finished.get_future();
        std::jthread fixture([&,scenario]{bool announced=false;try{
            QUdpSocket a,b;check(a.bind(QHostAddress(QHostAddress::LocalHost),quint16(0))&&b.bind(QHostAddress(QHostAddress::LocalHost),quint16(0)),"Lookup fixture bind failed");
            const Contact ca{QByteArray::fromHex("80000000000000000000000000000000"),0x7f000001,a.localPort(),4662,9};
            const Contact cb{QByteArray::fromHex("00000000000000000000000000000001"),0x7f000001,b.localPort(),4662,9};
            ready.set_value(ca);announced=true;check(a.waitForReadyRead(3000),"First-hop query missing");const auto request=a.receiveDatagram(100);
            check(request.data()==QByteArray::fromHex("e42114")+wire(destination)+wire(ca.id),"First-hop request identity wrong");
            auto answer=QByteArray::fromHex("e429")+wire(destination)+QByteArray(1,char(2))+contactBytes(cb)+contactBytes(ca);
            if(scenario==2)answer[2]=1;
            check(a.writeDatagram(answer,request.senderAddress(),request.senderPort())==answer.size(),"First-hop reply failed");
            if(scenario==0){check(b.waitForReadyRead(3000),"Discovered closer node not queried");const auto query=b.receiveDatagram(100);
                check(query.data()==QByteArray::fromHex("e42114")+wire(destination)+wire(cb.id),"Second-hop request wrong");
                const auto empty=QByteArray::fromHex("e529")+qCompress(wire(destination)+QByteArray(1,'\0')).mid(4);
                check(b.writeDatagram(empty,query.senderAddress(),query.senderPort())==empty.size(),"Second-hop reply failed");
            }
            finished.set_value();
        }catch(...){if(!announced)ready.set_exception(std::current_exception());finished.set_exception(std::current_exception());}});
        const auto result=findClosest(destination,{first.get()},3000,scenario==1?1:8,1000);done.get();
        if(scenario==0)check(result.queries==2&&result.responsive.size()==2&&!result.budgetExhausted&&result.responsive[0].id.endsWith(char(1)),"Two-hop lookup or XOR order failed");
        if(scenario==1)check(result.queries==1&&result.responsive.size()==1&&result.budgetExhausted,"Query budget ignored");
        if(scenario==2)check(result.queries==1&&result.responsive.isEmpty(),"Mismatched target marked responsive");
    }
    std::cout<<"Kad bounded iterative UDP lookup tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

#include "ed2k_server.h"
#include <QCoreApplication>
#include <QTcpServer>
#include <QtEndian>
#include <future>
#include <thread>
#include <iostream>
using namespace downloader::ed2k;
static void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
template<class F>void rejects(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected,"Expected rejection");}
static QByteArray frame(quint8 opcode,const QByteArray& body){QByteArray bytes(6,Qt::Uninitialized);bytes[0]=char(0xe3);qToLittleEndian<quint32>(quint32(body.size()+1),bytes.data()+1);bytes[5]=char(opcode);return bytes+body;}
static QByteArray readFrame(QTcpSocket& socket){QByteArray bytes;
    auto read=[&](qsizetype count){while(bytes.size()<count){if(!socket.bytesAvailable())check(socket.waitForReadyRead(3000),"Fixture read failed");bytes+=socket.read(count-bytes.size());}};
    read(5);const auto length=qFromLittleEndian<quint32>(bytes.constData()+1);check(length>0&&length<4096,"Fixture bound");read(5+length);return bytes;
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    const QByteArray hash(16,'h'),user(16,'u');
    check(requestSources(hash,123,false).payload==hash+QByteArray::fromHex("7b000000"),"32-bit query format");
    check(requestSources(hash,quint64(1)<<32,true).payload==hash+QByteArray::fromHex("000000000000000001000000"),"64-bit query format");
    rejects([&]{requestSources(hash,quint64(1)<<32,false);});
    QByteArray list=hash+QByteArray::fromHex("057f00000136120100000036127f00000136120000000036127f0000010000");
    auto parsed=std::get<QVector<ServerSource>>(parseSources({Protocol::Edonkey,0x42,list},hash));
    check(parsed.size()==2&&parsed[0].clientId==0x0100007f&&!parsed[0].needsCallback()&&parsed[1].needsCallback(),"Source ID endian/classification/deduplication");
    for(qsizetype length=0;length<list.size();++length)check(std::holds_alternative<InvalidPacket>(parseSources({Protocol::Edonkey,0x42,list.first(length)},hash)),"Truncated source list accepted");
    check(std::holds_alternative<InvalidPacket>(parseSources({Protocol::Edonkey,0x42,list+"x"},hash)),"Trailing source bytes accepted");
    for(int scenario=0;scenario<3;++scenario){
        std::promise<quint16> ready;auto port=ready.get_future();std::promise<void> finished;auto done=finished.get_future();
        std::jthread server([&,scenario]{bool announced=false;try{
            QTcpServer listener;check(listener.listen(QHostAddress::LocalHost,0),"Fixture listen failed");ready.set_value(listener.serverPort());announced=true;
            check(listener.waitForNewConnection(3000),"Fixture accept failed");std::unique_ptr<QTcpSocket> socket(listener.nextPendingConnection());
            const auto login=readFrame(*socket);
            const auto expected=user+QByteArray::fromHex("0000000036120200000002010001010041030100113c000000");
            check(login==frame(1,expected),"Unexpected server login wire bytes");
            if(scenario==2){socket->write(frame(0x05,{}));socket->flush();}
            else{
                socket->write(frame(0x38,QByteArray::fromHex("02004f4b"))+frame(0x40,QByteArray::fromHex("0403020100010000")));socket->flush();
                check(readFrame(*socket)==frame(0x19,hash+QByteArray::fromHex("7b000000")),"Unexpected source query wire bytes");
                auto reply=list;if(scenario==1)reply[0]='x';
                const auto bytes=frame(0x42,reply);
                // Send a split header/body through the actual socket.
                socket->write(bytes.first(3));socket->flush();socket->write(bytes.mid(3));socket->flush();
            }
            socket->disconnectFromHost();if(socket->state()!=QAbstractSocket::UnconnectedState)socket->waitForDisconnected(1000);
            finished.set_value();
        }catch(...){if(!announced)ready.set_exception(std::current_exception());finished.set_exception(std::current_exception());}});
        ServerConnection connection;bool rejected=false;
        try{const auto identity=connection.login(QHostAddress::LocalHost,port.get(),user,4662,"A",3000);
            check(identity.clientId==0x01020304&&identity.flags==0x100,"Login capabilities missing");
            const auto sources=connection.sources(hash,123,3000);check(sources.size()==2,"TCP source list mismatch");
        }catch(const std::runtime_error&){if(scenario==0)throw;rejected=true;}
        connection.close();done.get();check(scenario==0||rejected,"Invalid server response accepted");
    }
    std::cout<<"ed2k server login and source discovery tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

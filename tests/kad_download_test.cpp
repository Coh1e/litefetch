#include "kad_download.h"
#include "ed2k_hash.h"
#include "task_manager.h"
#include <QCoreApplication>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QtEndian>
#include <future>
#include <thread>
#include <iostream>
using namespace downloader;
static void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
static QByteArray wire(const QByteArray& id){QByteArray bytes(16,Qt::Uninitialized);for(int i=0;i<16;i+=4)qToLittleEndian(qFromBigEndian<quint32>(id.constData()+i),bytes.data()+i);return bytes;}
static void append(QByteArray& bytes,quint32 value){char raw[4];qToLittleEndian(value,raw);bytes.append(raw,4);}
static QByteArray frame(quint8 opcode,const QByteArray& body){QByteArray bytes(1,char(0xe3));append(bytes,quint32(body.size()+1));bytes.append(char(opcode));return bytes+body;}
static QByteArray readFrame(QTcpSocket& socket){QByteArray bytes;
    const auto read=[&](qsizetype n){while(bytes.size()<n){if(!socket.bytesAvailable())check(socket.waitForReadyRead(3000),"TCP fixture read failed");bytes+=socket.read(n-bytes.size());}};
    read(5);const auto n=qFromLittleEndian<quint32>(bytes.constData()+1);check(n>0&&n<65536,"TCP fixture frame budget");read(5+n);return bytes;
}
static void send(QTcpSocket& socket,const QByteArray& bytes){check(socket.write(bytes)==bytes.size(),"TCP fixture send failed");while(socket.bytesToWrite()>0)check(socket.waitForBytesWritten(3000),"TCP fixture flush failed");}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    QTemporaryDir dir(QDir::currentPath()+"/kad-download-XXXXXX");check(dir.isValid(),"Test directory failed");
    QByteArray data(600000,Qt::Uninitialized);for(qsizetype i=0;i<data.size();++i)data[i]=char((i*7+11)%251);
    ed2k::FileHasher hasher;hasher.add(data);const auto hash=hasher.result();const QByteArray sourceHash(16,'s');
    const auto nodeId=QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");
    for(int scenario=0;scenario<6;++scenario){
        std::promise<quint16> ready;auto port=ready.get_future();std::promise<void> finished;auto done=finished.get_future();
        std::jthread fixture([&,scenario]{bool announced=false;try{
            QUdpSocket udp;QTcpServer peer;check(udp.bind(QHostAddress(QHostAddress::LocalHost),quint16(0))&&peer.listen(QHostAddress::LocalHost,0),"Kad/TCP fixture bind failed");
            ready.set_value(udp.localPort());announced=true;
            const auto incoming=[&]{check(udp.waitForReadyRead(3000),"Kad fixture query missing");return udp.receiveDatagram(1024);};
            const auto reply=[&](const QNetworkDatagram& request,const QByteArray& bytes){check(udp.writeDatagram(bytes,request.senderAddress(),request.senderPort())==bytes.size(),"Kad fixture response failed");};
            auto query=incoming();check(query.data()==QByteArray::fromHex("e401"),"Bootstrap missing");reply(query,QByteArray::fromHex("e409")+wire(nodeId)+QByteArray::fromHex("3612090000"));
            query=incoming();check(query.data()==QByteArray::fromHex("e42114")+wire(hash)+wire(nodeId),"Iterative file-ID lookup missing");reply(query,QByteArray::fromHex("e429")+wire(hash)+QByteArray(1,'\0'));
            query=incoming();char size[10]{};qToLittleEndian<quint64>(quint64(data.size()),size+2);
            check(query.data()==QByteArray::fromHex("e434")+wire(hash)+QByteArray(size,10),"File source request missing");
            auto page=QByteArray::fromHex("e43b")+wire(nodeId)+wire(hash)+QByteArray::fromHex("0100")+wire(sourceHash)+QByteArray(1,char(4));
            page+=QByteArray::fromHex("090100ff");page.append(char(scenario==3?3:1));page+=QByteArray::fromHex("030100fe0100007f080100fd");
            char tcp[2];qToLittleEndian(peer.serverPort(),tcp);page.append(tcp,2);page+=QByteArray::fromHex("090100f3");page.append(char(scenario==2?4:0));reply(query,page);
            if(scenario==2||scenario==3){check(!peer.waitForNewConnection(250),"Unsupported source opened TCP connection");finished.set_value();return;}
            check(peer.waitForNewConnection(3000),"Kad source not handed to TCP engine");std::unique_ptr<QTcpSocket> socket(peer.nextPendingConnection());
            const auto hello=readFrame(*socket);check(hello[5]==1,"Peer hello missing");auto answer=hello.mid(7);answer.replace(0,16,scenario==1?QByteArray(16,'v'):sourceHash);send(*socket,frame(0x4c,answer));
            if(scenario==1){socket->waitForReadyRead(1000);check(socket->readAll().isEmpty(),"Identity mismatch reached file negotiation");finished.set_value();return;}
            check(readFrame(*socket)==frame(0x58,hash),"File confirmation missing");send(*socket,frame(0x59,hash+QByteArray::fromHex("0800746573742e62696e")));
            check(readFrame(*socket)==frame(0x54,hash),"Upload slot request missing");send(*socket,frame(0x55,{}));
            quint32 offset=0;int batches=0;
            while(offset<quint32(data.size())){const auto blocks=readFrame(*socket);check(blocks.size()==46&&blocks[5]==0x47&&blocks.mid(6,16)==hash,"Block batch identity wrong");QByteArray bytes;
                for(int i=0;i<3;++i){const auto begin=qFromLittleEndian<quint32>(blocks.constData()+22+4*i),end=qFromLittleEndian<quint32>(blocks.constData()+34+4*i);
                    if(!begin&&!end){check(offset==quint32(data.size()),"Early block padding");continue;}
                    check(begin==offset&&end>begin&&end<=quint32(data.size()),"Block coverage wrong");QByteArray body=hash;append(body,begin);append(body,end);auto part=data.mid(begin,end-begin);
                    if(scenario==4&&begin==0)part[0]^=1;body+=part;bytes+=frame(0x46,body);offset=end;
                }send(*socket,bytes);++batches;
            }
            check(batches>1,"Multiple request batches not exercised");if(scenario==0||scenario==5)check(readFrame(*socket)==frame(0x56,{}),"Upload slot release missing");finished.set_value();
        }catch(...){if(!announced)ready.set_exception(std::current_exception());finished.set_exception(std::current_exception());}});
        ed2k::ServerDownload request;request.userHash=QByteArray(16,'u');request.fileHash=hash;request.size=quint64(data.size());request.nickname="Fixture";
        request.destination=dir.filePath(QString::number(scenario)+".bin");request.checkpointDirectory=dir.filePath(QString::number(scenario)+"-cache");request.operationTimeoutMs=3000;
        kad::DownloadOptions options;options.bootstrapSeeds={{QHostAddress::LocalHost,port.get()}};options.discoveryTimeoutMs=4000;
        bool rejected=false;try{
            if(scenario==5){
                Ed2kRuntimeOptions runtime;runtime.useKad=true;runtime.kadSeeds=options.bootstrapSeeds;runtime.userHash=request.userHash;runtime.nickname=request.nickname;runtime.operationTimeoutMs=3000;runtime.kadDiscoveryTimeoutMs=4000;
                TaskManager manager(dir.filePath("kad-tasks.sqlite"),1,{},nullptr,{},runtime);
                const auto link="ed2k://|file|test.bin|"+QString::number(data.size())+"|"+QString::fromLatin1(hash.toHex())+"|/";
                const auto task=manager.add(std::get<DownloadRequest>(parseRequest(link)),request.destination);check(task.added,"Kad task rejected by shared queue");
                QElapsedTimer timer;timer.start();while(manager.tasks()[0].state!=TaskState::Completed&&timer.elapsed()<10000){QCoreApplication::processEvents();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
                check(manager.tasks()[0].state==TaskState::Completed&&manager.tasks()[0].received==data.size(),"Kad shared task did not complete");
            }else{const auto result=kad::downloadFile(request,options);check(result.bytes==quint64(data.size())&&result.attemptedSources==1,"Kad download accounting wrong");}
        }
        catch(const std::runtime_error&){if(scenario==0)throw;rejected=true;}
        done.get();if(scenario==0||scenario==5){QFile result(request.destination);check(result.open(QIODevice::ReadOnly)&&result.readAll()==data,"Kad-discovered download differs");}
        else check(rejected&&!QFile::exists(request.destination),"Invalid Kad source published output");
    }
    std::cout<<"Kad bootstrap to verified TCP file tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

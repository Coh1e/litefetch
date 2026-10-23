#include "ed2k_download.h"
#include "ed2k_hash.h"
#include "ed2k_blocks.h"
#include "ed2k_part_cache.h"
#include "ed2k_storage_error.h"
#include <QCoreApplication>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QtEndian>
#include <future>
#include <thread>
#include <iostream>
using namespace downloader::ed2k;
static void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
template<class F>void rejects(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected,"Expected rejection");}
static void append(QByteArray& bytes,quint32 value){char raw[4];qToLittleEndian(value,raw);bytes.append(raw,4);}
static QByteArray frame(quint8 op,const QByteArray& payload){QByteArray bytes(1,char(0xe3));append(bytes,quint32(payload.size()+1));bytes.append(char(op));return bytes+payload;}
static QByteArray readFrame(QTcpSocket& socket){QByteArray bytes;
    const auto read=[&](qsizetype count){while(bytes.size()<count){if(!socket.bytesAvailable())check(socket.waitForReadyRead(3000),"Fixture read failed");bytes+=socket.read(count-bytes.size());}};
    read(5);const auto n=qFromLittleEndian<quint32>(bytes.constData()+1);check(n>0&&n<65536,"Fixture request too large");read(5+n);return bytes;
}
static std::unique_ptr<QTcpSocket> accept(QTcpServer& listener){check(listener.waitForNewConnection(3000),"Fixture accept failed");return std::unique_ptr<QTcpSocket>(listener.nextPendingConnection());}
static void send(QTcpSocket& socket,const QByteArray& bytes){check(socket.write(bytes)==bytes.size(),"Fixture write failed");while(socket.bytesToWrite()>0)check(socket.waitForBytesWritten(3000),"Fixture flush failed");}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    QTemporaryDir dir(QDir::currentPath()+"/ed2k-download-XXXXXX");check(dir.isValid(),"Test directory failed");
    QByteArray data(qsizetype(partSize+12345),Qt::Uninitialized);for(qsizetype i=0;i<data.size();++i)data[i]=char((i*13+7)%251);
    FileHasher hasher;hasher.add(data);const auto hash=hasher.result();
    for(int scenario=0;scenario<7;++scenario){
        std::promise<quint16> ready;auto port=ready.get_future();std::promise<void> finished;auto done=finished.get_future();
        std::jthread fixture([&,scenario]{bool announced=false;try{
            QTcpServer server,bad,good;check(server.listen(QHostAddress::LocalHost,0)&&bad.listen(QHostAddress::LocalHost,0)&&good.listen(QHostAddress::LocalHost,0),"Fixture listen failed");
            ready.set_value(server.serverPort());announced=true;
            auto control=accept(server);check(readFrame(*control)[5]==1,"Missing server login");
            send(*control,frame(0x40,QByteArray::fromHex("7f00000100000000")));
            QByteArray query=hash;append(query,quint32(data.size()));check(readFrame(*control)==frame(0x19,query),"Wrong requested file");
            QByteArray list=hash;list.append(char(2));
            for(auto endpoint:{bad.serverPort(),good.serverPort()}){list+=QByteArray::fromHex("7f000001");char raw[2];qToLittleEndian(endpoint,raw);list.append(raw,2);}
            send(*control,frame(0x42,list));control->disconnectFromHost();
            for(int source=0;source<2;++source){auto socket=accept(source==0?bad:good);
                const auto hello=readFrame(*socket);check(hello[5]==1,"Missing peer hello");send(*socket,frame(0x4c,hello.mid(7)));
                check(readFrame(*socket)==frame(0x58,hash),"Missing file confirmation");
                if(source==0&&(scenario<2||scenario==5)){send(*socket,frame(0x48,hash));socket->disconnectFromHost();continue;}
                send(*socket,frame(0x59,hash+QByteArray::fromHex("0800746573742e62696e")));
                check(readFrame(*socket)==frame(0x4f,hash),"Missing multipart status query");
                const auto status=scenario==2?(source==0?QByteArray::fromHex("020001"):QByteArray::fromHex("020002")):QByteArray::fromHex("0000");
                send(*socket,frame(0x50,hash+status));
                check(readFrame(*socket)==frame(0x54,hash),"Missing upload request");send(*socket,frame(0x55,{}));
                if((source==0||scenario<2)&&scenario!=6){
                    check(readFrame(*socket)==frame(0x51,hash),"Missing hashset request");
                    QByteArray hashes=hash+QByteArray::fromHex("0200");
                    hashes+=QCryptographicHash::hash(QByteArrayView(data).first(qsizetype(partSize)),QCryptographicHash::Md4);
                    hashes+=QCryptographicHash::hash(QByteArrayView(data).sliced(qsizetype(partSize)),QCryptographicHash::Md4);
                    send(*socket,frame(0x52,hashes));
                }
                const quint64 start=(source==1&&scenario>=2)||scenario==6?partSize:0;
                const quint64 limit=(scenario==1||(scenario==2&&source==0))?partSize:quint64(data.size());
                quint64 offset=start;int batches=0;
                while(offset<limit){
                    const auto request=readFrame(*socket);check(request[5]==0x47&&request.size()==46&&request.mid(6,16)==hash,"Invalid block request");
                    QByteArray reply;
                    for(int i=0;i<3;++i){const auto begin=qFromLittleEndian<quint32>(request.constData()+22+4*i),end=qFromLittleEndian<quint32>(request.constData()+34+4*i);
                        if(begin==0&&end==0){check(i>0,"Empty request batch");continue;}
                        check(begin==offset&&end>begin&&end-begin<=blockSize&&end<=limit&&begin/partSize==(end-1)/partSize,"Request skipped or redownloaded verified bytes");
                        const auto emittedEnd=scenario==4&&source==0&&begin==partSize?begin+1000:end;
                        QByteArray body=hash;append(body,begin);append(body,emittedEnd);auto bytes=data.mid(begin,emittedEnd-begin);
                        if((scenario==1&&begin==0)||(scenario==3&&source==0&&begin==partSize))bytes[0]^=1;
                        body+=bytes;reply+=frame(0x46,body);offset=end;
                    }
                    send(*socket,reply);++batches;
                }
                if(limit-start>3*blockSize)check(batches>1,"Test failed to exercise multiple batches");
                if(scenario!=1&&!(scenario>=3&&source==0))check(readFrame(*socket)==frame(0x56,{}),"Upload slot not released");
                if(scenario==6){
                    if(socket->state()!=QAbstractSocket::UnconnectedState)
                        check(socket->waitForDisconnected(1500),"Storage failure did not close active source");
                    check(!good.hasPendingConnections()&&!good.waitForNewConnection(250),"Storage failure tried next source");
                    break;
                }
                socket->disconnectFromHost();
            }
            finished.set_value();
        }catch(...){if(!announced)ready.set_exception(std::current_exception());finished.set_exception(std::current_exception());}});
        ServerDownload request;request.server=QHostAddress::LocalHost;request.serverPort=port.get();request.userHash=QByteArray(16,'u');request.fileHash=hash;
        request.nickname="Fixture";request.destination=dir.filePath(QString::number(scenario)+".bin");request.size=quint64(data.size());request.operationTimeoutMs=3000;
        quint64 reserved=0;
        if(scenario==0||scenario==5)request.downloadBudget=[&](quint64 maximum){const auto grant=qMin<quint64>(maximum,37001);reserved+=grant;return grant;};
        if(scenario==5||scenario==6){
            request.checkpointDirectory=dir.filePath(scenario==5?"resume-cache":"storage-failure-cache");
            FileReceiver seed(dir.filePath("resume-seed.bin"),hash,request.size);
            for(quint64 offset=0;offset<partSize;){const auto end=qMin(partSize,offset+blockSize);
                seed.accept({hash,{offset,end},data.mid(qsizetype(offset),qsizetype(end-offset))});offset=end;}
            PartCache cache(request.checkpointDirectory,hash,request.size);
            cache.storeHashes({QCryptographicHash::hash(QByteArrayView(data).first(qsizetype(partSize)),QCryptographicHash::Md4),
                QCryptographicHash::hash(QByteArrayView(data).sliced(qsizetype(partSize)),QCryptographicHash::Md4)});
            cache.storePart(seed,0);
        }
        if(scenario==6)request.progress=[&](quint64 received){
            if(received!=request.size)return;
            const auto blocked=QDir(request.checkpointDirectory).filePath(QString::fromLatin1(hash.toHex())+"-"+QString::number(request.size)+"-1.part");
            check(QDir().mkdir(blocked),"Could not block next cache part destination");
        };
        bool rejected=false;
        try{const auto result=downloadFromServer(request);check(result.bytes==quint64(data.size())&&result.attemptedSources==2,"Source fallback or byte count failed");}
        catch(const StorageError&){if(scenario!=6)throw;rejected=true;}
        catch(const std::runtime_error&){if(scenario!=1)throw;rejected=true;}
        done.get();
        if(scenario==0)check(reserved==quint64(data.size()),"Budget did not cover exact requested payload");
        if(scenario==5)check(reserved==12345,"Restored verified part consumed network budget");
        if(scenario==6){
            check(rejected&&!QFile::exists(request.destination),"Failed cache write published result or lost storage type");
            const auto cached=QDir(request.checkpointDirectory).filePath(QString::fromLatin1(hash.toHex())+"-"+QString::number(request.size)+"-0.part");
            QFile file(cached);check(file.open(QIODevice::ReadOnly)&&file.readAll()==data.first(qsizetype(partSize)),"Storage failure damaged previously verified cache");
        }
        else if(scenario!=1){QFile result(request.destination);check(result.open(QIODevice::ReadOnly)&&result.readAll()==data,"Discovered-source output differs");}
        else check(rejected&&!QFile::exists(request.destination),"Corrupt source published");
        std::stop_source cancelled;cancelled.request_stop();request.destination=dir.filePath("cancelled.bin");rejects([&]{downloadFromServer(request,cancelled.get_token());});
    }
    check(QDir(dir.path()).entryList({"*.part"},QDir::Files|QDir::Hidden).isEmpty(),"Failed-source staging leaked");
    std::cout<<"ed2k server discovery to verified file tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

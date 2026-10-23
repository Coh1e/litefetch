#include "ed2k_transfer.h"
#include "ed2k_hash.h"
#include <QCoreApplication>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QDir>
#include <QtEndian>
#include <future>
#include <thread>
#include <iostream>
using namespace downloader::ed2k;
static void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
static void append(QByteArray& bytes,quint32 n){char data[4];qToLittleEndian(n,data);bytes.append(data,4);}
static QByteArray frame(quint8 opcode,const QByteArray& body){QByteArray bytes(1,char(0xe3));append(bytes,quint32(body.size()+1));bytes.append(char(opcode));return bytes+body;}
static QByteArray readFrame(QTcpSocket& socket){QByteArray bytes;
    auto read=[&](qsizetype count){while(bytes.size()<count){if(!socket.bytesAvailable())check(socket.waitForReadyRead(3000),"Fixture read timed out");bytes+=socket.read(count-bytes.size());}};
    read(5);const auto length=qFromLittleEndian<quint32>(bytes.constData()+1);check(length>0&&length<65536,"Fixture frame bound");read(5+length);return bytes;
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    QTemporaryDir dir(QDir::currentPath()+"/ed2k-transfer-XXXXXX");check(dir.isValid(),"Test directory failed");
    QByteArray data(500000,Qt::Uninitialized);for(qsizetype i=0;i<data.size();++i)data[i]=char((i*17+31)%251);
    FileHasher hasher;hasher.add(data);const auto hash=hasher.result();
    const QVector<BlockRange> ranges{{0,180000},{180000,360000},{360000,500000}};
    for(int scenario=0;scenario<4;++scenario){
        std::promise<quint16> ready;auto port=ready.get_future();std::promise<void> finished;auto done=finished.get_future();
        std::jthread server([&,scenario]{bool announced=false;try{
            QTcpServer listener;check(listener.listen(QHostAddress::LocalHost,0),"Fixture listen failed");ready.set_value(listener.serverPort());announced=true;
            check(listener.waitForNewConnection(3000),"Fixture accept failed");std::unique_ptr<QTcpSocket> socket(listener.nextPendingConnection());
            const auto hello=readFrame(*socket);check(hello[5]==1,"Missing hello");
            QByteArray answer=hello.mid(7);socket->write(frame(0x4c,answer));socket->flush();
            check(readFrame(*socket)==frame(0x58,hash),"Missing file-name request before transfer");
            QByteArray filename=hash;filename.append(char(8));filename.append(char(0));filename+="test.bin";
            socket->write(frame(0x59,filename));socket->flush();
            check(readFrame(*socket)==frame(0x54,hash),"Missing upload-slot request");
            QByteArray rank;append(rank,17);socket->write(frame(0x5c,rank)+frame(0x55,{}));socket->flush();
            QByteArray expected=hash;for(auto r:ranges)append(expected,quint32(r.begin));for(auto r:ranges)append(expected,quint32(r.end));
            check(readFrame(*socket)==frame(0x47,expected),"Unexpected request wire bytes");
            QByteArray response;
            for(qsizetype index=ranges.size();index-->0;){const auto r=ranges[index];const quint64 middle=(r.begin+r.end)/2;
                for(auto part:QVector<BlockRange>{{middle,r.end},{r.begin,middle}}){
                    QByteArray payload=(scenario==1?QByteArray(16,'x'):hash);append(payload,quint32(part.begin));append(payload,quint32(part.end));
                    auto bytes=data.mid(qsizetype(part.begin),qsizetype(part.end-part.begin));if(scenario==2)bytes[0]^=1;
                    payload+=bytes;response+=frame(0x46,payload);
                    if(index==ranges.size()-1&&part.begin==middle){
                        if(scenario==3)payload[payload.size()-1]^=1;
                        response+=frame(0x46,payload); // Identical duplicate or conflicting duplicate.
                    }
                }
            }
            check(socket->write(response)==response.size(),"Fixture response write failed");
            while(socket->bytesToWrite()>0&&socket->state()==QAbstractSocket::ConnectedState)
                if(!socket->waitForBytesWritten(3000))break;
            socket->disconnectFromHost();if(socket->state()!=QAbstractSocket::UnconnectedState)socket->waitForDisconnected(1000);
            finished.set_value();
        }catch(...){if(!announced)ready.set_exception(std::current_exception());finished.set_exception(std::current_exception());}});
        const auto target=dir.filePath(QString::number(scenario)+".bin");
        PeerConnection peer;FileReceiver receiver(target,hash,quint64(data.size()));
        peer.connectHello(QHostAddress::LocalHost,port.get(),makeHello(QByteArray(16,'u'),0,4662,"Fixture"),3000);
        DownloadQueue queue(hash,quint64(data.size()));waitForUploadSlot(peer,queue,3000);
        check(queue.state()==QueueState::Ready&&queue.filename()=="test.bin","Upload negotiation did not complete");
        bool rejected=false;
        try{const auto count=receiveRequestedBlocks(peer,receiver,hash,ranges,quint64(data.size()),false,3000);
            check(count==quint64(data.size()),"Transfer double-counted duplicate");receiver.publish();
        }catch(const std::runtime_error&){if(scenario==0)throw;rejected=true;}
        peer.close();done.get();
        if(scenario==0){QFile file(target);check(file.open(QIODevice::ReadOnly)&&file.readAll()==data,"Transferred file differs");}
        else check(rejected&&!QFile::exists(target),"Invalid transfer was published");
    }
    std::cout<<"ed2k TCP block-to-disk transfer tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

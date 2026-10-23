#include "ed2k_peer.h"
#include "ed2k_queue.h"
#include "ed2k_transfer.h"
#include "ed2k_hash.h"
#include "ed2k_download.h"
#include "kad_client.h"
#include "bandwidth_limiter.h"
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDeadlineTimer>
#include <iostream>
#include <cstdlib>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
using namespace downloader::ed2k;
int main(int argc,char** argv){
#ifdef _MSC_VER
    _set_error_mode(_OUT_TO_STDERR);
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ERROR,_CRTDBG_FILE_STDERR);
#endif
    std::set_terminate([]{std::cerr<<"Interop probe terminated: ";if(const auto error=std::current_exception())try{std::rethrow_exception(error);}catch(const std::exception& e){std::cerr<<e.what();}catch(...){std::cerr<<"unknown exception";}std::cerr<<'\n';std::_Exit(99);});
    QCoreApplication app(argc,argv);QJsonObject report;QString reportPath;
    const auto save=[&]{if(reportPath.isEmpty())return;QSaveFile output(reportPath);const auto bytes=QJsonDocument(report).toJson();
        if(output.open(QIODevice::WriteOnly)){output.write(bytes);output.commit();}};
    try{
        if(argc!=5&&argc!=6)throw std::runtime_error("Usage: interop-probe port fixture destination report [kad-port]");
        bool ok=false;const auto port=QString::fromLocal8Bit(argv[1]).toUShort(&ok);if(!ok||!port)throw std::runtime_error("Invalid probe port");
        reportPath=QString::fromLocal8Bit(argv[4]);report={{"reference","aMule 3.1.0"},{"stage","fixture"},{"completed",false}};
        if(argc==6){
            const auto udpPort=QString::fromLocal8Bit(argv[5]).toUShort(&ok);
            if(!ok||!udpPort)throw std::runtime_error("Invalid Kad probe port");
            report["stage"]="kad-bootstrap";save();
            const auto boot=downloader::kad::bootstrap(QHostAddress::LocalHost,udpPort,3000);
            if(boot.id.size()!=16||boot.tcpPort!=port)throw std::runtime_error("Reference Kad identity/port mismatch");
            report["kadBootstrapAccepted"]=true;report["kadNodeId"]=QString::fromLatin1(boot.id.toHex());
            report["kadVersion"]=boot.version;report["kadBootstrapContacts"]=boot.contacts.size();
            report["stage"]="kad-lookup";save();
            const downloader::kad::Contact contact{boot.id,QHostAddress(QHostAddress::LocalHost).toIPv4Address(),udpPort,port,boot.version};
            const auto contacts=downloader::kad::lookup(contact,QByteArray::fromHex("112233445566778899aabbccddeeff00"),3000);
            report["kadLookupAccepted"]=true;report["kadLookupContacts"]=contacts.size();save();
        }
        QFile fixture(QString::fromLocal8Bit(argv[2]));if(!fixture.open(QIODevice::ReadOnly)||fixture.size()>20*1024*1024)throw std::runtime_error("Invalid bounded fixture");
        const auto bytes=fixture.readAll();FileHasher hasher;hasher.add(bytes);const auto hash=hasher.result();
        report["bytes"]=bytes.size();report["fileHash"]=QString::fromLatin1(hash.toHex());save();
        PeerConnection peer;report["stage"]="hello";save();peer.connectTransport(QHostAddress::LocalHost,port,5000);
        peer.send(makeHello(QByteArray::fromHex("1234567890abcdef1234567890abcdef"),0,4663,"NativeDownloaderInterop"),5000);
        const auto answer=peer.receive(5000,65536);report["helloProtocol"]=int(answer.protocol);report["helloOpcode"]=answer.opcode;
        const auto parsed=parseHello(answer);if(!std::holds_alternative<Hello>(parsed))throw std::runtime_error("Reference hello was rejected");
        report["helloAccepted"]=true;report["stage"]="file-and-upload-queue";save();
        const auto referenceUserHash=std::get<Hello>(parsed).userHash;
        DownloadQueue queue(hash,quint64(bytes.size()));peer.send(queue.begin(),5000);report["stage"]="waiting-file-answer";save();QJsonArray packets;QDeadlineTimer deadline(15000);
        while(queue.state()!=QueueState::Ready){
            if(deadline.hasExpired()||packets.size()>128)throw std::runtime_error("Reference queue did not grant upload slot");
            const auto packet=peer.receive(int(deadline.remainingTime()),65536);
            packets.append(QJsonObject{{"protocol",int(packet.protocol)},{"opcode",packet.opcode},{"payloadSize",packet.payload.size()},{"payload",QString::fromLatin1(packet.payload.left(128).toHex())}});
            report["packets"]=packets;save();const auto reply=queue.consume(packet);if(reply)peer.send(*reply,5000);
            if(queue.state()==QueueState::Unavailable)throw std::runtime_error("Reference did not share fixture");
        }
        report["stage"]="hashset";save();const auto hashes=requestHashset(peer,hash,quint64(bytes.size()),10000);
        report["partHashCount"]=hashes.size();report["stage"]="blocks";save();FileReceiver receiver(QString::fromLocal8Bit(argv[3]),hash,quint64(bytes.size()));
        quint64 offset=0;while(offset<quint64(bytes.size())){QVector<BlockRange> batch;
            for(int i=0;i<3&&offset<quint64(bytes.size());++i){const auto end=qMin<quint64>(quint64(bytes.size()),qMin(offset+blockSize,(offset/partSize+1)*partSize));batch.append({offset,end});offset=end;}
            receiveRequestedBlocks(peer,receiver,hash,batch,quint64(bytes.size()),false,15000);
        }
        for(quint64 part=0;part<(quint64(bytes.size())+partSize-1)/partSize;++part)if(!receiver.verifyPart(part,hashes[qsizetype(part)]))throw std::runtime_error("Reference part hash mismatch");
        receiver.publish();peer.send({Protocol::Edonkey,0x56,{}},1000);peer.close();
        QFile result(QString::fromLocal8Bit(argv[3]));if(!result.open(QIODevice::ReadOnly)||result.readAll()!=bytes)throw std::runtime_error("Reference transfer bytes differ");
        report["manualTransferCompleted"]=true;report["stage"]="download-coordinator";save();
        ServerDownload run;run.userHash=QByteArray::fromHex("2234567890abcdef1234567890abcdef");run.fileHash=hash;run.size=quint64(bytes.size());run.nickname="NativeDownloaderCore";
        run.destination=QString::fromLocal8Bit(argv[3])+".coordinator";run.checkpointDirectory=QFileInfo(reportPath).absolutePath()+"/native-cache";run.operationTimeoutMs=10000;
        run.sourceProvider=[&]{return QVector<PeerEndpoint>{{QHostAddress::LocalHost,port,referenceUserHash}};};
        downloader::BandwidthLimiter limiter;limiter.setRate(256*1024);std::atomic_bool stopped=false;
        quint64 reserved=0;run.downloadBudget=[&](quint64 maximum){const auto grant=quint64(limiter.acquire(qint64(maximum),stopped));reserved+=grant;return grant;};
        QElapsedTimer elapsed;elapsed.start();
        const auto transfer=download(run);QFile actual(run.destination);
        report["coordinatorLimitBytesPerSecond"]=256*1024;report["coordinatorElapsedMs"]=elapsed.elapsed();report["coordinatorReservedBytes"]=qint64(reserved);
        if(reserved!=quint64(bytes.size())||elapsed.elapsed()<qint64(bytes.size())*1000/(256*1024)-250)throw std::runtime_error("Coordinator request budget did not limit transfer");
        if(transfer.bytes!=quint64(bytes.size())||!actual.open(QIODevice::ReadOnly)||actual.readAll()!=bytes)throw std::runtime_error("Coordinator interoperability bytes differ");
        report["coordinatorCompleted"]=true;report["stage"]="complete";report["completed"]=true;save();std::cout<<"aMule reference file transfer and coordinator passed\n";return 0;
    }catch(const std::exception& error){report["error"]=QString::fromLatin1(error.what());save();std::cerr<<error.what()<<'\n';return 1;}}

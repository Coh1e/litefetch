#include "ed2k_download.h"
#include "ed2k_transfer.h"
#include "ed2k_hash.h"
#include "ed2k_part_cache.h"
#include "disk_space.h"
#include "ed2k_storage_error.h"
#include <QtEndian>
#include <QFileInfo>
#include <limits>
#include <stdexcept>
#include <QSet>
namespace downloader::ed2k {
DownloadResult downloadFromServer(const ServerDownload& r,std::stop_token stop){return download(r,stop);}
DownloadResult download(const ServerDownload& r,std::stop_token stop){
    if((!r.sourceProvider&&(r.server.isNull()||!r.serverPort))||!r.incomingPort||r.userHash.size()!=16||r.fileHash.size()!=16||
       !r.size||r.size>std::numeric_limits<quint32>::max()||r.destination.isEmpty()||
       r.operationTimeoutMs<1||r.operationTimeoutMs>60000||r.maxSources<1||r.maxSources>255)
        throw std::invalid_argument("Invalid initial ed2k download configuration");
    const auto cancelled=[&]{return stop.stop_requested()||(r.cancelled&&r.cancelled());};
    const auto checkStop=[&]{if(cancelled())throw std::runtime_error("ed2k download cancelled");};
    checkStop();
    std::unique_ptr<PartCache> cache;
    if(!r.checkpointDirectory.isEmpty())cache=std::make_unique<PartCache>(r.checkpointDirectory,r.fileHash,r.size,cancelled);
    if(QFileInfo::exists(r.destination)){
        if(!cache||!cache->recoverPublication(r.destination))throw DestinationError("Destination already exists without a matching publication receipt");
        checkStop();if(r.progress)r.progress(r.size);cache->clear(r.deferPublicationAcknowledgement);return {r.size,0};
    }
    const auto parts=(r.size+partSize-1)/partSize;
    // Restoration copies cached parts into a new staging file; the full target
    // therefore needs additional space even when every cache part is present.
    requireDiskSpace(r.destination,qint64(r.size));
    FileReceiver receiver(r.destination,r.fileHash,r.size,cancelled);
    QVector<bool> verified(qsizetype(parts),false);QVector<QByteArray> hashes;
    if(cache){
        hashes=cache->restore(receiver,verified);checkStop();
        bool complete=true;for(const bool part:verified)complete&=part;
        if(r.progress)r.progress(receiver.received());
        if(complete){cache->preparePublication(r.destination);receiver.publish();cache->clear(r.deferPublicationAcknowledgement);return {receiver.received(),0};}
    }
    QVector<PeerEndpoint> sources;quint32 clientId=0;
    if(r.sourceProvider)sources=r.sourceProvider();
    else{
        ServerConnection server;
        server.setCancellationCheck(cancelled);
        clientId=server.login(r.server,r.serverPort,r.userHash,r.incomingPort,r.nickname,r.operationTimeoutMs).clientId;
        checkStop();const auto found=server.sources(r.fileHash,r.size,r.operationTimeoutMs);server.close();
        for(const auto source:found)if(!source.needsCallback())sources.append({QHostAddress(qbswap(source.clientId)),source.port,{}});
    }
    checkStop();if(sources.size()>300)throw std::runtime_error("Too many discovered ed2k sources");
    int attempts=0;QSet<quint64> tried;
    for(const auto& source:sources){
        checkStop();const auto& address=source.address;
        if(address.protocol()!=QAbstractSocket::IPv4Protocol||!address.toIPv4Address()||address.isMulticast()||address==QHostAddress::Broadcast||!source.port||
           (!source.expectedUserHash.isEmpty()&&source.expectedUserHash.size()!=16))continue;
        const auto endpoint=(quint64(address.toIPv4Address())<<16)|source.port;
        if(tried.contains(endpoint))continue;tried.insert(endpoint);
        if(attempts>=r.maxSources)break;++attempts;
        PeerConnection peer;quint64 active=parts;bool persisting=false;
        peer.setCancellationCheck(cancelled);
        try{
            const auto hello=peer.connectHello(address,source.port,makeHello(r.userHash,clientId,r.incomingPort,r.nickname),r.operationTimeoutMs);
            if(!source.expectedUserHash.isEmpty()&&hello.userHash!=source.expectedUserHash)throw std::runtime_error("Discovered ed2k peer identity mismatch");
            checkStop();DownloadQueue queue(r.fileHash,r.size);waitForUploadSlot(peer,queue,r.operationTimeoutMs);checkStop();
            bool useful=false;for(quint64 part=0;part<parts;++part)useful|=!verified[qsizetype(part)]&&queue.hasPart(part);
            if(useful&&hashes.isEmpty()){
                hashes=requestHashset(peer,r.fileHash,r.size,r.operationTimeoutMs);
                if(cache){persisting=true;cache->storeHashes(hashes);persisting=false;}
            }
            for(quint64 part=0;part<parts;++part){
                if(verified[qsizetype(part)]||!queue.hasPart(part))continue;
                active=part;quint64 offset=part*partSize;
                const auto partEnd=qMin(r.size,offset+partSize);
                auto targetRequired=r.size-receiver.received();
                if(cache){
                    QStorageInfo targetVolume(QFileInfo(r.destination).absolutePath());
                    QStorageInfo cacheVolume(r.checkpointDirectory);
                    const auto cacheRequired=partEnd-offset;
                    if(targetVolume.isValid()&&cacheVolume.isValid()&&targetVolume.rootPath()==cacheVolume.rootPath())
                        targetRequired+=cacheRequired;
                    else requireDiskSpace(r.checkpointDirectory,qint64(cacheRequired));
                }
                requireDiskSpace(r.destination,qint64(targetRequired));
                while(offset<partEnd){
                    checkStop();QVector<BlockRange> batch;
                    auto batchEnd=partEnd;
                    if(r.downloadBudget){
                        const auto maximum=qMin(partEnd-offset,3*blockSize);
                        const auto granted=r.downloadBudget(maximum);checkStop();
                        if(!granted||granted>maximum)throw std::runtime_error("Invalid ed2k download budget grant");
                        batchEnd=offset+granted;
                    }
                    for(int count=0;count<3&&offset<batchEnd;++count){
                        const auto end=qMin(batchEnd,offset+blockSize);
                        batch.append({offset,end});offset=end;
                    }
                    receiveRequestedBlocks(peer,receiver,r.fileHash,batch,r.size,false,r.operationTimeoutMs);
                    if(r.progress)r.progress(receiver.received());
                }
                checkStop();
                if(!receiver.verifyPart(part,hashes[qsizetype(part)]))throw std::runtime_error("Source sent a corrupt ed2k part");
                if(cache){persisting=true;cache->storePart(receiver,part);persisting=false;}
                verified[qsizetype(part)]=true;active=parts;
            }
            bool complete=true;for(const bool part:verified)complete&=part;
            checkStop();if(complete){
                if(cache){persisting=true;cache->preparePublication(r.destination);persisting=false;}
                receiver.publish();
            }
            try{peer.send({Protocol::Edonkey,0x56,{}},r.operationTimeoutMs);}catch(const std::exception&){/* Closing also releases the slot. */}
            peer.close();if(complete){if(cache)cache->clear(r.deferPublicationAcknowledgement);return {receiver.received(),attempts};}
        }catch(const StorageError&){
            peer.close();throw;
        }catch(const DestinationError&){
            peer.close();throw;
        }catch(const DiskSpaceError&){
            peer.close();throw;
        }catch(const std::runtime_error&){
            peer.close();checkStop();if(persisting||QFileInfo::exists(r.destination))throw;
            // A partial or rejected part cannot conflict with another source.
            // Previously verified parts remain untouched in the shared receiver.
            if(active<parts&&!verified[qsizetype(active)])receiver.discardPart(active);
            if(r.progress)r.progress(receiver.received());
        }
    }
    throw std::runtime_error("Direct ed2k sources did not complete all verified parts");
}
}

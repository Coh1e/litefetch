#include "ed2k_task.h"
#include "bandwidth_limiter.h"
#include "ed2k_download.h"
#include "ed2k_part_cache.h"
#include "kad_download.h"
#include "disk_space.h"
#include "ed2k_storage_error.h"
#include <QCryptographicHash>
#include <QDir>
#include <limits>
#include <stdexcept>
namespace downloader {
namespace {
QByteArray fileHash(const DownloadRequest& r){
    const auto identity=r.identities.value(0).split(':');
    if(r.protocol!=Protocol::Ed2k||identity.size()!=3||!r.expectedSize||!*r.expectedSize||*r.expectedSize>std::numeric_limits<quint32>::max())
        throw std::invalid_argument("Unsupported ed2k task identity or size");
    const auto hash=QByteArray::fromHex(identity[1].toLatin1());
    if(hash.size()!=16)throw std::invalid_argument("Invalid ed2k task hash");return hash;
}
}
bool validEd2kOptions(const Ed2kRuntimeOptions& o){
    if(!o.incomingPort||o.userHash.size()!=16||o.nickname.isEmpty()||o.nickname.toUtf8().size()>1024||o.nickname.contains(QChar::Null)||
       o.operationTimeoutMs<1||o.operationTimeoutMs>60000||o.kadDiscoveryTimeoutMs<3||o.kadDiscoveryTimeoutMs>60000||o.kadSeeds.size()>8)return false;
    for(const auto& seed:o.kadSeeds)if(seed.first.protocol()!=QAbstractSocket::IPv4Protocol||!seed.first.toIPv4Address()||seed.first.isMulticast()||seed.first==QHostAddress::Broadcast||!seed.second)return false;
    return o.useKad?!o.kadSeeds.isEmpty():!o.server.isNull()&&o.serverPort;
}
QString ed2kTaskCachePath(const QString& root,const QString& id){return QDir(root).filePath(QString::fromLatin1(QCryptographicHash::hash(id.toUtf8(),QCryptographicHash::Sha256).toHex()));}
void discardEd2kCache(const QString& root,const QString& id,const DownloadRequest& request){
    const auto hash=fileHash(request);ed2k::PartCache cache(ed2kTaskCachePath(root,id),hash,*request.expectedSize);cache.clear();
}
TransferResult runEd2kTask(const DownloadRequest& input,const QString& destination,const QString& id,
    const QString& root,const Ed2kRuntimeOptions& options,const std::atomic_bool& cancelled,
    const std::function<void(TransferProgress)>& progress,BandwidthLimiter* limiter){
    quint64 received=0;
    try{
        const auto parsed=parseRequest(input.source);const auto* request=std::get_if<DownloadRequest>(&parsed);
        if(!request||request->protocol!=Protocol::Ed2k||!validEd2kOptions(options))
            return {TransferStatus::Failed,TransferError::InvalidRequest,QStringLiteral("请先配置有效的 ed2k 找源方式。")};
        ed2k::ServerDownload run;run.fileHash=fileHash(*request);run.size=*request->expectedSize;
        run.server=options.server;run.serverPort=options.serverPort;run.incomingPort=options.incomingPort;
        run.userHash=options.userHash;run.nickname=options.nickname;run.operationTimeoutMs=options.operationTimeoutMs;
        run.destination=destination;run.checkpointDirectory=ed2kTaskCachePath(root,id);
        run.deferPublicationAcknowledgement=true;
        run.cancelled=[&]{return cancelled.load();};
        if(limiter)run.downloadBudget=[&](quint64 maximum){return quint64(limiter->acquire(qint64(maximum),cancelled));};
        run.progress=[&](quint64 count){received=count;if(progress)progress({qint64(count),qint64(run.size)});};
        kad::DownloadOptions kadOptions;kadOptions.bootstrapSeeds=options.kadSeeds;kadOptions.discoveryTimeoutMs=options.kadDiscoveryTimeoutMs;
        const auto result=options.useKad?kad::downloadFile(run,kadOptions):ed2k::downloadFromServer(run);
        if(progress)progress({qint64(result.bytes),qint64(run.size)});
        return {TransferStatus::Completed,TransferError::None,{},0,qint64(result.bytes)};
    }catch(const ed2k::StorageError& error){
        if(cancelled.load())return {TransferStatus::Cancelled,TransferError::None,{},0,qint64(received)};
        return {TransferStatus::Failed,TransferError::Disk,QStringLiteral("ed2k 文件或缓存读写失败，完整缓存已保留。\n%1").arg(QString::fromUtf8(error.what())),0,qint64(received)};
    }catch(const ed2k::DestinationError& error){
        if(cancelled.load())return {TransferStatus::Cancelled,TransferError::None,{},0,qint64(received)};
        return {TransferStatus::Failed,TransferError::Destination,QStringLiteral("ed2k 目标文件冲突，未覆盖已有文件。\n%1").arg(QString::fromUtf8(error.what())),0,qint64(received)};
    }catch(const DiskSpaceError& error){
        if(cancelled.load())return {TransferStatus::Cancelled,TransferError::None,{},0,qint64(received)};
        return {TransferStatus::Failed,TransferError::Disk,error.message(),0,qint64(received)};
    }catch(const std::exception&){
        if(cancelled.load())return {TransferStatus::Cancelled,TransferError::None,QStringLiteral("已停止，完整校验分块已保留。"),0,qint64(received)};
        return {TransferStatus::Failed,TransferError::Network,QStringLiteral("ed2k 下载未完成，已保存的完整分块可继续下载。"),0,qint64(received)};
    }
}
}

#include "ed2k_part_cache.h"
#include "ed2k_hashset.h"
#include "ed2k_hash.h"
#include "disk_space.h"
#include "ed2k_storage_error.h"
#include <QDir>
#include <QSaveFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <stdexcept>
namespace downloader::ed2k {
namespace {
Packet hashsetPacket(const QByteArray& hash,quint64 size,const QVector<QByteArray>& hashes){
    const quint64 expected=size<partSize?1:size/partSize+1;
    if(quint64(hashes.size())!=expected||expected>65535)throw std::invalid_argument("Invalid cached hashset count");
    QByteArray body=hash;char bytes[2];qToLittleEndian<quint16>(size<partSize?0:quint16(expected),bytes);body.append(bytes,2);
    if(size<partSize){if(hashes.first()!=hash)throw std::invalid_argument("Invalid small-file cache hash");}
    else for(const auto& value:hashes){if(value.size()!=16)throw std::invalid_argument("Invalid cached part hash");body+=value;}
    return {Protocol::Edonkey,0x52,body};
}
}
PartCache::PartCache(QString directory,QByteArray hash,quint64 size,std::function<bool()> cancelled):hash_(std::move(hash)),size_(size),cancelled_(std::move(cancelled)){
    checkStop();
    if(directory.isEmpty()||hash_.size()!=16||!size_||size_/partSize+1>65535)throw std::invalid_argument("Invalid part cache identity");
    QDir dir(directory);if(!dir.exists()&&!dir.mkpath("."))throw StorageError("Cannot create part cache directory");
    prefix_=dir.absoluteFilePath(QString::fromLatin1(hash_.toHex())+"-"+QString::number(size_));
    lock_=std::make_unique<QLockFile>(prefix_+".lock");lock_->setStaleLockTime(0);
    if(!lock_->tryLock())throw StorageError("Part cache is already in use");
}
QString PartCache::partPath(quint64 index)const{return prefix_+"-"+QString::number(index)+".part";}
void PartCache::checkStop()const{if(cancelled_&&cancelled_())throw std::runtime_error("ed2k cache operation cancelled");}
QVector<QByteArray> PartCache::restore(FileReceiver& receiver,QVector<bool>& verified){
    checkStop();
    const auto count=(size_+partSize-1)/partSize;
    if(verified.size()!=qsizetype(count)||receiver.received()!=0)throw std::invalid_argument("Cache restore requires an empty receiver");
    verified.fill(false);
    QFile metadata(prefix_+".hashset");
    if(!metadata.open(QIODevice::ReadOnly)||metadata.size()>18+16*65535)return {};
    auto parsed=parseHashset({Protocol::Edonkey,0x52,metadata.readAll()},hash_,size_);
    const auto* hashes=std::get_if<QVector<QByteArray>>(&parsed);if(!hashes)return {};
    hashes_=*hashes;
    for(quint64 index=0;index<count;++index){
        checkStop();
        const auto begin=index*partSize,length=qMin(partSize,size_-begin);
        QFile part(partPath(index));if(!part.open(QIODevice::ReadOnly)||part.size()!=qint64(length))continue;
        quint64 offset=0;
        while(offset<length){checkStop();const auto bytes=part.read(qint64(qMin<quint64>(blockSize,length-offset)));
            if(bytes.isEmpty())break;
            receiver.accept({hash_,{begin+offset,begin+offset+quint64(bytes.size())},bytes});offset+=quint64(bytes.size());
        }
        if(offset!=length){receiver.discardPart(index);continue;}
        verified[qsizetype(index)]=receiver.verifyPart(index,hashes_[qsizetype(index)]);
    }
    return hashes_;
}
void PartCache::storeHashes(const QVector<QByteArray>& hashes){
    checkStop();
    if(hashes_==hashes)return;
    const auto packet=hashsetPacket(hash_,size_,hashes);
    if(!std::holds_alternative<QVector<QByteArray>>(parseHashset(packet,hash_,size_)))throw std::invalid_argument("Unverified cache hashset");
    QSaveFile output(prefix_+".hashset");output.setDirectWriteFallback(false);
    if(!output.open(QIODevice::WriteOnly)||output.write(packet.payload)!=packet.payload.size())throw StorageError("Cannot persist part hashset");
    checkStop();if(!output.commit())throw StorageError("Cannot persist part hashset");
    hashes_=hashes;
}
void PartCache::storePart(FileReceiver& receiver,quint64 index){
    checkStop();
    if(hashes_.isEmpty()||index>=(size_+partSize-1)/partSize)throw std::invalid_argument("Part cache has no verified hashes");
    if(!receiver.verifyPart(index,hashes_[qsizetype(index)]))throw std::runtime_error("Cannot cache a corrupt part");
    // QSaveFile keeps an existing cache file until commit. Its logical length
    // must not be counted as reusable storage for the replacement.
    requireDiskSpace(partPath(index),qint64(qMin(partSize,size_-index*partSize)));
    QSaveFile output(partPath(index));output.setDirectWriteFallback(false);
    if(!output.open(QIODevice::WriteOnly))throw StorageError("Cannot create cached part");
    receiver.copyPart(index,output);checkStop();if(!output.commit())throw StorageError("Cannot persist cached part");
}
void PartCache::preparePublication(const QString& destination){
    checkStop();
    const QJsonObject object{{"version",1},{"destination",QFileInfo(destination).absoluteFilePath()},
        {"hash",QString::fromLatin1(hash_.toHex())},{"size",QString::number(size_)}};
    const auto bytes=QJsonDocument(object).toJson(QJsonDocument::Compact);
    QSaveFile receipt(prefix_+".publishing");receipt.setDirectWriteFallback(false);
    if(!receipt.open(QIODevice::WriteOnly)||receipt.write(bytes)!=bytes.size())throw StorageError("Cannot persist publication receipt");
    checkStop();if(!receipt.commit())throw StorageError("Cannot persist publication receipt");
}
bool PartCache::recoverPublication(const QString& destination){
    checkStop();
    QFile receipt(prefix_+".publishing");
    if(!receipt.open(QIODevice::ReadOnly)||receipt.size()>16384)return false;
    const auto document=QJsonDocument::fromJson(receipt.readAll());if(!document.isObject())return false;
    const auto object=document.object();
    if(object["version"].toInt()!=1||object["destination"].toString()!=QFileInfo(destination).absoluteFilePath()||
       object["hash"].toString()!=QString::fromLatin1(hash_.toHex())||object["size"].toString()!=QString::number(size_))return false;
    QFile output(destination);if(!output.open(QIODevice::ReadOnly)||output.size()!=qint64(size_))return false;
    FileHasher hasher;
    while(!output.atEnd()){checkStop();const auto bytes=output.read(65536);if(bytes.isEmpty())return false;hasher.add(bytes);}
    return hasher.size()==size_&&hasher.result()==hash_;
}
void PartCache::clear(bool keepPublicationReceipt){
    for(quint64 index=0;index<(size_+partSize-1)/partSize;++index)QFile::remove(partPath(index));
    QFile::remove(prefix_+".hashset");
    if(!keepPublicationReceipt)QFile::remove(prefix_+".publishing");
}
}

#include "ed2k_storage.h"
#include "ed2k_hash.h"
#include "ed2k_storage_error.h"
#include <QFileInfo>
#include <QDir>
#include <limits>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <filesystem>
#endif
#include <stdexcept>
namespace downloader::ed2k {
FileReceiver::FileReceiver(QString destination,QByteArray hash,quint64 size,std::function<bool()> cancelled)
    :destination_(QFileInfo(destination).absoluteFilePath()),hash_(std::move(hash)),size_(size),
     file_(std::make_unique<QTemporaryFile>(QFileInfo(destination_).absolutePath()+"/.ed2k-XXXXXX.part")),cancelled_(std::move(cancelled)){
    if(hash_.size()!=16||size_>quint64(std::numeric_limits<qint64>::max())||
       QFileInfo(destination_).fileName().isEmpty())throw std::invalid_argument("Invalid receiving file");
    if(QFileInfo::exists(destination_))throw DestinationError("Destination already exists");
    checkStop();
    if(!file_->open())throw StorageError("Cannot create staging file");
}
void FileReceiver::checkStop()const{if(cancelled_&&cancelled_())throw std::runtime_error("ed2k file operation cancelled");}
quint64 FileReceiver::accept(const DataBlock& block){
    checkStop();
    if(failed_||published_)throw std::logic_error("Receiver is closed");
    const auto begin=block.range.begin,end=block.range.end;
    if(block.fileHash!=hash_||begin>=end||end>size_||end-begin>blockSize||
       begin/partSize!=(end-1)/partSize||quint64(block.bytes.size())!=end-begin)
        throw std::invalid_argument("Invalid received block");
    auto first=ranges_.lower_bound(begin);
    if(first!=ranges_.begin()&&std::prev(first)->second>=begin)--first;
    auto last=first;
    quint64 mergedBegin=begin,mergedEnd=end,overlap=0;
    for(;last!=ranges_.end()&&last->first<=mergedEnd;++last){
        checkStop();
        const auto left=qMax(begin,last->first),right=qMin(end,last->second);
        if(left<right){
            if(!file_->seek(qint64(left)))throw StorageError("Cannot read staging file");
            const auto existing=file_->read(qint64(right-left));
            if(existing.size()!=qint64(right-left))throw StorageError("Cannot read staging file");
            if(existing!=block.bytes.sliced(qsizetype(left-begin),qsizetype(right-left)))
                throw std::runtime_error("Conflicting received bytes");
            overlap+=right-left;
        }
        mergedBegin=qMin(mergedBegin,last->first);mergedEnd=qMax(mergedEnd,last->second);
    }
    if(ranges_.size()-std::distance(first,last)>=65536)throw std::runtime_error("Too many fragmented ranges");
    // Allocate interval bookkeeping before mutating the file.
    std::map<quint64,quint64> insertion;insertion.emplace(mergedBegin,mergedEnd);
    if(!file_->seek(qint64(begin))||file_->write(block.bytes)!=block.bytes.size()){
        failed_=true;throw StorageError("Cannot write staging file");
    }
    ranges_.erase(first,last);ranges_.insert(insertion.extract(insertion.begin()));
    received_+=end-begin-overlap;return end-begin-overlap;
}
void FileReceiver::discardPart(quint64 index){
    if(failed_||published_)throw std::logic_error("Receiver is closed");
    if(!size_||index>(size_-1)/partSize)throw std::invalid_argument("Invalid data part index");
    const auto begin=index*partSize,end=qMin(size_,begin+partSize);
    std::map<quint64,quint64> next;quint64 removed=0;
    for(const auto& [left,right]:ranges_){
        if(right<=begin||left>=end){next.emplace(left,right);continue;}
        removed+=qMin(right,end)-qMax(left,begin);
        if(left<begin)next.emplace(left,begin);
        if(right>end)next.emplace(end,right);
    }
    ranges_.swap(next);received_-=removed;
}
bool FileReceiver::verifyPart(quint64 index,const QByteArray& digest){
    checkStop();
    if(failed_||published_)throw std::logic_error("Receiver is closed");
    if(!size_||index>(size_-1)/partSize||digest.size()!=16)throw std::invalid_argument("Invalid part verification");
    const auto begin=index*partSize,end=qMin(size_,begin+partSize);
    auto entry=ranges_.upper_bound(begin);
    if(entry==ranges_.begin()||std::prev(entry)->second<end)throw std::runtime_error("Part is incomplete");
    if(!file_->flush()||!file_->seek(qint64(begin))){failed_=true;throw StorageError("Cannot verify part");}
    QCryptographicHash hash(QCryptographicHash::Md4);quint64 remaining=end-begin;
    while(remaining){checkStop();const auto bytes=file_->read(qint64(qMin<quint64>(remaining,65536)));
        if(bytes.isEmpty()){failed_=true;throw StorageError("Cannot read part for verification");}
        hash.addData(bytes);remaining-=quint64(bytes.size());
    }
    if(hash.result()==digest)return true;
    discardPart(index);return false;
}
void FileReceiver::copyPart(quint64 index,QIODevice& output){
    checkStop();
    if(failed_||published_)throw std::logic_error("Receiver is closed");
    if(!size_||index>(size_-1)/partSize)throw std::invalid_argument("Invalid data part index");
    const auto begin=index*partSize,end=qMin(size_,begin+partSize);
    auto entry=ranges_.upper_bound(begin);
    if(entry==ranges_.begin()||std::prev(entry)->second<end)throw std::runtime_error("Part is incomplete");
    if(!file_->flush()||!file_->seek(qint64(begin)))throw StorageError("Cannot read cached part");
    quint64 remaining=end-begin;
    while(remaining){checkStop();const auto bytes=file_->read(qint64(qMin<quint64>(remaining,65536)));
        if(bytes.isEmpty()||output.write(bytes)!=bytes.size())throw StorageError("Cannot copy cached part");
        remaining-=quint64(bytes.size());
    }
}
void FileReceiver::publish(){
    checkStop();
    if(failed_||published_)throw std::logic_error("Receiver is closed");
    if(received_!=size_)throw std::runtime_error("File is incomplete");
    if(!file_->flush()||!file_->seek(0)){failed_=true;throw StorageError("Cannot verify staging file");}
    FileHasher hasher;
    while(!file_->atEnd()){
        checkStop();
        const auto bytes=file_->read(65536);
        if(bytes.isEmpty()){failed_=true;throw StorageError("Cannot read staging file");}
        hasher.add(bytes);
    }
    if(hasher.size()!=size_||hasher.result()!=hash_){failed_=true;throw std::runtime_error("File identity mismatch");}
    checkStop(); // Last cancellable point before atomic publication.
    // Do not use QFile::rename: its copy fallback can expose a partially
    // copied final file. Never allow replacement or cross-volume copying.
#ifdef Q_OS_WIN
    const auto staging=QDir::toNativeSeparators(file_->fileName()).toStdWString();
    const auto destination=QDir::toNativeSeparators(destination_).toStdWString();
    file_->setAutoRemove(false);
    file_.reset(); // QTemporaryFile::close alone can retain the native handle.
    if(!MoveFileExW(staging.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH)){
        const auto reason=GetLastError();
        failed_=true;QFile::remove(QString::fromStdWString(staging));
        if(reason==ERROR_ALREADY_EXISTS||reason==ERROR_FILE_EXISTS)throw DestinationError("Destination appeared before publication");
        throw StorageError("Cannot publish file without overwriting");
    }
#else
    std::error_code error;
    std::filesystem::create_hard_link(std::filesystem::path(file_->fileName().toStdWString()),
        std::filesystem::path(destination_.toStdWString()),error);
    if(error==std::errc::file_exists)throw DestinationError("Destination appeared before publication");
    if(error)throw StorageError("Cannot publish file without overwriting");
    // Auto-remove deletes only the temporary name; the published link survives.
    file_->close();
#endif
    published_=true;
}
}

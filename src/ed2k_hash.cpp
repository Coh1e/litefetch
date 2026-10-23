#include "ed2k_hash.h"
#include <limits>
#include <stdexcept>
namespace downloader::ed2k {
FileHasher::FileHasher(){
    if(!QCryptographicHash::supportsAlgorithm(QCryptographicHash::Md4))throw std::runtime_error("Qt MD4 is unavailable");
}
void FileHasher::add(QByteArrayView bytes){
    if(finished_)throw std::logic_error("ed2k hash already finalized");
    if(quint64(bytes.size())>std::numeric_limits<quint64>::max()-size_)throw std::overflow_error("ed2k size overflow");
    while(!bytes.empty()){
        const auto count=static_cast<qsizetype>(qMin<quint64>(partSize-partBytes_,bytes.size()));
        part_.addData(bytes.first(count));partBytes_+=count;size_+=count;bytes=bytes.sliced(count);
        if(partBytes_==partSize){root_.addData(part_.result());part_.reset();partBytes_=0;++fullParts_;}
    }
}
QByteArray FileHasher::result(){
    if(!finished_){
        if(!fullParts_)result_=part_.result();
        else{root_.addData(part_.result());result_=root_.result();}
        finished_=true;
    }
    return result_;
}
}

#include "ed2k_queue.h"
#include "ed2k_hash.h"
#include <QtEndian>
#include <stdexcept>
namespace downloader::ed2k {
DownloadQueue::DownloadQueue(QByteArray hash,quint64 size):hash_(std::move(hash)),size_(size){
    if(hash_.size()!=16||size_==0||size_/partSize+1>65535)throw std::invalid_argument("Invalid queued file identity or size");
}
Packet DownloadQueue::begin(){
    if(state_!=QueueState::Idle)throw std::logic_error("File request already started");
    state_=QueueState::AwaitingName;return {Protocol::Edonkey,0x58,hash_};
}
bool DownloadQueue::hasPart(quint64 index)const{
    if(index>=(size_+partSize-1)/partSize)return false;
    return complete_||(!parts_.isEmpty()&&(quint8(parts_[qsizetype(index/8)])&(1u<<(index%8))));
}
std::optional<Packet> DownloadQueue::consume(const Packet& packet){
    if(state_==QueueState::Idle||state_==QueueState::Ready||state_==QueueState::Unavailable||state_==QueueState::Failed)
        throw std::logic_error("No pending file negotiation");
    try{
        const auto& p=packet.payload;
        if(packet.protocol==Protocol::Edonkey&&packet.opcode==0x48){
            if(p!=hash_)throw std::runtime_error("Invalid file-unavailable identity");
            state_=QueueState::Unavailable;return {};
        }
        if(state_==QueueState::AwaitingName&&packet.protocol==Protocol::Edonkey&&packet.opcode==0x59){
            if(p.size()<18||p.first(16)!=hash_)throw std::runtime_error("Invalid remote filename identity");
            const auto length=qFromLittleEndian<quint16>(p.constData()+16);
            if(length==0||length>4096||p.size()!=18+length||p.mid(18).contains('\0'))throw std::runtime_error("Invalid remote filename");
            name_=p.mid(18);
            if(size_>partSize){state_=QueueState::AwaitingStatus;return Packet{Protocol::Edonkey,0x4f,hash_};}
            complete_=true;state_=QueueState::Queued;return Packet{Protocol::Edonkey,0x54,hash_};
        }
        if(state_==QueueState::AwaitingStatus&&packet.protocol==Protocol::Edonkey&&packet.opcode==0x50){
            if(p.size()<18||p.first(16)!=hash_)throw std::runtime_error("Invalid remote file status identity");
            const auto count=qFromLittleEndian<quint16>(p.constData()+16);
            if(count==0){if(p.size()!=18)throw std::runtime_error("Invalid complete file status");complete_=true;}
            else{
                if(count!=size_/partSize+1||p.size()!=18+(count+7)/8)throw std::runtime_error("Invalid remote part bitmap");
                parts_=p.mid(18);
                bool any=false;for(quint64 i=0;i<(size_+partSize-1)/partSize;++i)any|=hasPart(i);
                if(!any){state_=QueueState::Unavailable;return {};}
            }
            state_=QueueState::Queued;return Packet{Protocol::Edonkey,0x54,hash_};
        }
        if(state_==QueueState::Queued){
            if(packet.protocol==Protocol::Edonkey&&packet.opcode==0x55&&p.isEmpty()){
                state_=QueueState::Ready;rank_=0;return {};
            }
            if(packet.protocol==Protocol::Edonkey&&packet.opcode==0x5c&&p.size()==4){rank_=qFromLittleEndian<quint32>(p.constData());return {};}
            if(packet.protocol==Protocol::Emule&&packet.opcode==0x60&&p.size()==12){rank_=qFromLittleEndian<quint16>(p.constData());return {};}
        }
        throw std::runtime_error("Unexpected file negotiation packet");
    }catch(...){state_=QueueState::Failed;throw;}
}
}

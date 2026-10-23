#include "ed2k_transfer.h"
#include "ed2k_hash.h"
#include <QDeadlineTimer>
#include <stdexcept>
namespace downloader::ed2k {
QVector<QByteArray> requestHashset(PeerConnection& peer,const QByteArray& hash,quint64 size,int timeout){
    if(hash.size()!=16||timeout<1||timeout>60000)throw std::invalid_argument("Invalid hashset request");
    if(size<partSize)return {hash};
    if(size/partSize+1>65535)throw std::invalid_argument("Hashset exceeds protocol count");
    QDeadlineTimer timer(timeout);
    const auto remaining=[&]{const auto n=timer.remainingTime();if(n<=0)throw std::runtime_error("Hashset request timed out");return static_cast<int>(n);};
    try{
        peer.send({Protocol::Edonkey,0x51,hash},remaining());
        auto result=parseHashset(peer.receive(remaining(),18+16*65535),hash,size);
        if(auto* hashes=std::get_if<QVector<QByteArray>>(&result))return std::move(*hashes);
        throw std::runtime_error("Peer returned an invalid hashset");
    }catch(...){peer.close();throw;}
}
void waitForUploadSlot(PeerConnection& peer,DownloadQueue& queue,int timeout){
    if(timeout<1||timeout>60000)throw std::invalid_argument("Invalid queue timeout");
    QDeadlineTimer deadline(timeout);
    const auto remaining=[&]{const auto value=deadline.remainingTime();
        if(value<=0)throw std::runtime_error("ed2k upload queue timed out");return static_cast<int>(value);};
    try{
        peer.send(queue.begin(),remaining());
        for(int count=0;count<512;++count){
            const auto reply=queue.consume(peer.receive(remaining(),8192));
            if(reply)peer.send(*reply,remaining());
            if(queue.state()==QueueState::Ready)return;
            if(queue.state()==QueueState::Unavailable)throw std::runtime_error("Remote peer has no usable file parts");
        }
        throw std::runtime_error("ed2k queue packet budget exceeded");
    }catch(...){peer.close();throw;}
}
quint64 receiveRequestedBlocks(PeerConnection& peer,FileReceiver& receiver,
    const QByteArray& hash,const QVector<BlockRange>& ranges,quint64 size,
    bool largeOffsets,int timeout){
    if(timeout<1||timeout>60000)throw std::invalid_argument("Invalid block batch timeout");
    const auto request=requestBlocks(hash,ranges,size,largeOffsets);
    QDeadlineTimer deadline(timeout);
    const auto remaining=[&]{const auto value=deadline.remainingTime();
        if(value<=0)throw std::runtime_error("ed2k block batch timed out");
        return static_cast<int>(value);};
    auto missing=ranges;quint64 added=0;
    try{
        peer.send(request,remaining());
        for(int packets=0;!missing.isEmpty();++packets){
            if(packets>=4096)throw std::runtime_error("ed2k block batch packet budget exceeded");
            const auto packet=peer.receive(remaining(),static_cast<quint32>(blockSize+32));
            auto decoded=parseDataBlock(packet,hash,ranges,size);
            const auto* block=std::get_if<DataBlock>(&decoded);
            if(!block)throw std::runtime_error("Invalid or unsolicited ed2k transfer data");
            if(packet.protocol==Protocol::Emule&&!largeOffsets)
                throw std::runtime_error("Unnegotiated large ed2k transfer data");
            QVector<BlockRange> next;
            for(const auto range:missing){
                if(block->range.end<=range.begin||block->range.begin>=range.end){next.append(range);continue;}
                if(range.begin<block->range.begin)next.append({range.begin,block->range.begin});
                if(block->range.end<range.end)next.append({block->range.end,range.end});
            }
            // Bound bookkeeping as well as packets for tiny, fragmented replies.
            if(next.size()>4096)throw std::runtime_error("ed2k block batch fragmentation exceeded");
            added+=receiver.accept(*block);missing=std::move(next);
        }
        return added;
    }catch(...){peer.close();throw;}
}
}

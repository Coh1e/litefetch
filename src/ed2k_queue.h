#pragma once
#include "ed2k_wire.h"
#include <optional>
namespace downloader::ed2k {
enum class QueueState {Idle,AwaitingName,AwaitingStatus,Queued,Ready,Unavailable,Failed};
// Event-driven state, independent of sockets and UI. Filename bytes are advisory,
// never a local path. The task owns its trusted destination and expected size.
class DownloadQueue {
public:
    DownloadQueue(QByteArray hash,quint64 size);
    Packet begin();
    std::optional<Packet> consume(const Packet& packet);
    QueueState state()const{return state_;}
    quint32 rank()const{return rank_;}
    QByteArray filename()const{return name_;}
    bool hasPart(quint64 index)const;
private:
    QByteArray hash_,name_,parts_;
    quint64 size_;
    quint32 rank_=0;
    bool complete_=false;
    QueueState state_=QueueState::Idle;
};
}

#pragma once
#include "ed2k_peer.h"
#include "ed2k_storage.h"
#include "ed2k_queue.h"
#include "ed2k_hashset.h"
namespace downloader::ed2k {
// Bounded initial negotiation on an already handshaken peer. Long queue waits
// can instead drive DownloadQueue with the task scheduler's packet events.
void waitForUploadSlot(PeerConnection& peer,DownloadQueue& queue,int timeoutMs=10000);
QVector<QByteArray> requestHashset(PeerConnection& peer,const QByteArray& hash,quint64 size,int timeoutMs=10000);
// A single requested batch after the remote peer grants an upload slot.
// Queue negotiation, discovery and task scheduling are handled above this layer.
quint64 receiveRequestedBlocks(PeerConnection& peer,FileReceiver& receiver,
    const QByteArray& hash,const QVector<BlockRange>& ranges,quint64 fileSize,
    bool largeOffsets,int timeoutMs=10000);
}

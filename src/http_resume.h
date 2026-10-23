#pragma once
#include "transfer.h"
namespace downloader {
// Checkpoints belong to one persisted UUID task; source and destination are bound
// in its manifest. Stop retains a verified prefix; explicit cancellation discards it.
QString partialPath(const QString& destination, const QString& taskId);
bool discardPartial(const QString& destination, const QString& taskId);
void acknowledgePublished(const QString& destination, const QString& taskId);
TransferResult transferResumable(const DownloadRequest& request, const QString& destination,
    const QString& taskId, const std::atomic_bool& stopped,
    const std::function<void(TransferProgress)>& progress = {},BandwidthLimiter* limiter=nullptr);
}

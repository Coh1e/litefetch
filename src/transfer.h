#pragma once
#include "download_request.h"
#include <atomic>
#include <functional>

namespace downloader {
class BandwidthLimiter;
enum class TransferStatus { Completed, Cancelled, Failed };
// Persisted identifiers: append new values, never renumber existing entries.
enum class TransferError { None=0, InvalidRequest=1, Destination=2, Network=3, Http=4, Tls=5, Disk=6, Internal=7, ResourceChanged=8 };
enum class TransferPhase { Receiving, FetchingMetadata, CheckingFiles, Seeding, ChoosingFiles };
struct TransferProgress { qint64 received; qint64 total; TransferPhase phase = TransferPhase::Receiving;
    qint64 uploaded=-1; qint64 uploadBytesPerSecond=0; };
struct TransferResult {
    TransferStatus status = TransferStatus::Failed;
    TransferError error = TransferError::None;
    QString message;
    long responseCode = 0;
    qint64 received = 0;
    qint64 uploaded = -1;
};

// Blocking worker API. Call off the GUI thread; progress runs on the caller thread.
// The destination must not exist. An incomplete transfer never publishes it.
TransferResult transfer(const DownloadRequest& request, const QString& destination,
    const std::atomic_bool& cancelled,
    const std::function<void(TransferProgress)>& progress = {},BandwidthLimiter* limiter=nullptr);
}

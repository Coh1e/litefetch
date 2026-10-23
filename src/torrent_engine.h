#pragma once
#include "transfer.h"
#include "seeding_policy.h"
#include <QVector>
#include <memory>

namespace downloader {
struct TorrentFile { QString path; qint64 size; bool padding = false; };
struct TorrentMetadata {
    QString name;
    QString magnet;
    QStringList identities;
    QVector<TorrentFile> files;
    qint64 total = 0;
};
struct TorrentEngineOptions {
    bool localOnly = false;
    int uploadBytesPerSecond = 128 * 1024;
    int downloadBytesPerSecond = 0;
};
struct TorrentRunOptions {
    QVector<int> filePriorities;
    QVector<QPair<QString, quint16>> peers;
    QByteArray metainfo;
    std::function<QVector<int>(const TorrentMetadata&, const std::atomic_bool&)> selectFiles;
    SeedingPolicy seeding;
};
struct TorrentRateLimits {int download=0;int upload=128*1024;};
using TorrentInspection = std::variant<TorrentMetadata, InvalidRequest>;
TorrentInspection inspectTorrent(const QString& path);
TorrentInspection inspectTorrentData(QByteArrayView bytes);
QString validateTorrentSelection(const TorrentMetadata& metadata, const QVector<int>& priorities);

// One session shared by concurrent jobs, with an independent alert pump.
// Blocking run() belongs on worker threads. Join all runs before destruction.
class TorrentEngine {
public:
    explicit TorrentEngine(TorrentEngineOptions options = {});
    ~TorrentEngine();
    TorrentEngine(const TorrentEngine&) = delete;
    TorrentEngine& operator=(const TorrentEngine&) = delete;
    // Applies to all jobs in this shared session, including existing peers.
    void setRateLimits(TorrentRateLimits limits);
    TorrentRateLimits rateLimits() const;
    quint16 listeningPort() const;
    TransferResult run(const DownloadRequest& request, const QString& directory,
        const QString& taskId, const std::atomic_bool& stopped,
        const std::function<void(TransferProgress)>& progress = {}, TorrentRunOptions options = {});
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}

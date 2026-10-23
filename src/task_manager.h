#pragma once
#include "task_store.h"
#include "transfer.h"
#include "torrent_engine.h"
#include "ed2k_task.h"
#include "bandwidth_limiter.h"
#include <QObject>
#include <map>
#include <thread>
#include <future>
#include <mutex>
#include <stdexcept>

namespace downloader {
using TransferRunner = std::function<TransferResult(const DownloadRequest&, const QString&,
    const std::atomic_bool&, const std::function<void(TransferProgress)>&)>;
struct AddTaskResult { QString id; bool added = false; QString error; };
struct TorrentRuntimeOptions {
    TorrentEngineOptions engine;
    QVector<QPair<QString, quint16>> peers;
    SeedingPolicy seeding;
};

class TaskManager : public QObject {
    Q_OBJECT
public:
    explicit TaskManager(const QString& databasePath, int concurrency = 3,
        TransferRunner runner = {}, QObject* parent = nullptr, TorrentRuntimeOptions torrent = {},Ed2kRuntimeOptions ed2k = {});
    ~TaskManager() override;
    const QVector<Task>& tasks() const { return tasks_; }
    AddTaskResult add(const DownloadRequest& request, const QString& destination, int priority = 0,
        QVector<int> filePriorities = {});
    bool selectTorrentFiles(const QString& id, const QVector<int>& priorities);
    bool cancel(const QString& id);
    bool pause(const QString& id);
    bool resume(const QString& id);
    bool canReseed(const QString& id) const;
    bool retry(const QString& id);
    bool remove(const QString& id);
    bool setPriority(const QString& id, int priority);
    bool configureEd2k(Ed2kRuntimeOptions options);
    void setHttpDownloadLimit(qint64 bytesPerSecond){httpDownloadLimiter_.setRate(bytesPerSecond);}
    qint64 httpDownloadLimit() const{return httpDownloadLimiter_.rate();}
    void setTorrentRateLimits(TorrentRateLimits limits);
    TorrentRateLimits torrentRateLimits();
    // Manager-thread API; existing runs keep their policy snapshot.
    void setSeedingPolicy(SeedingPolicy policy){
        if(!validSeedingPolicy(policy))throw std::invalid_argument("Invalid seeding policy");
        torrentOptions_.seeding=policy;
        QStringList ids;
        for(const auto& task:tasks_)if(task.state==TaskState::Completed&&task.request.protocol==Protocol::BitTorrent)ids.append(task.id);
        for(const auto& id:ids)emit changed(id);
    }
    SeedingPolicy seedingPolicy()const{return torrentOptions_.seeding;}
    BrowserHandoff prepareBrowserHandoff(const QString& origin, const QString& requestId,
        const DownloadRequest& request, const QString& destination, qint64 now);
    BrowserHandoff commitBrowserHandoff(const QString& origin, const QString& requestId, qint64 now);
    std::optional<BrowserHandoff> queryBrowserHandoff(const QString& origin, const QString& requestId, qint64 now);
    std::optional<BrowserHandoff> abortBrowserHandoff(const QString& origin, const QString& requestId, qint64 now);
signals:
    void changed(const QString& id);
    void storageError(const QString& message);
    void torrentFilesReady(const QString& id, const downloader::TorrentMetadata& metadata);
private:
    struct Worker { std::shared_ptr<std::atomic_bool> cancelled; std::jthread thread; };
    TaskStore store_;
    QVector<Task> tasks_;
    std::map<QString, Worker> workers_;
    TransferRunner runner_;
    BandwidthLimiter httpDownloadLimiter_;
    struct Selection { TorrentMetadata metadata; std::promise<QVector<int>> answer; };
    std::map<QString, std::shared_ptr<Selection>> selections_;
    TorrentRuntimeOptions torrentOptions_;
    Ed2kRuntimeOptions ed2kOptions_;
    QString ed2kCacheRoot_;
    std::mutex torrentMutex_;
    std::shared_ptr<TorrentEngine> torrentEngine_;
    int concurrency_;
    bool closing_ = false;
    bool storageFailed_ = false;
    Task* find(const QString& id);
    bool persist(const Task& task);
    void schedule();
    void finish(const QString& id, const TransferResult& result);
    std::shared_ptr<TorrentEngine> torrentEngine();
    QVector<int> requestTorrentSelection(const QString& id, const TorrentMetadata& metadata, const std::atomic_bool& stopped);
};
QString taskStateText(TaskState state);
}

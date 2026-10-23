#pragma once
#include "download_request.h"
#include "transfer.h"
#include <QVector>
#include <memory>

struct sqlite3;
namespace downloader {
enum class TaskState { Queued, Running, Cancelling, Completed, Cancelled, Failed, Interrupted, Pausing, Paused };
struct Task {
    QString id;
    DownloadRequest request;
    QString destination;
    QString identity;
    TaskState state = TaskState::Queued;
    int priority = 0;
    qint64 created = 0;
    qint64 received = 0;
    qint64 total = 0;
    QString error;
    QVector<int> filePriorities;
    QByteArray torrentMetainfo;
    TransferPhase phase = TransferPhase::Receiving;
    TransferError errorType = TransferError::None;
    // Live metrics; the BT core owns durable transfer statistics in resume data.
    qint64 uploaded = -1;
    qint64 uploadBytesPerSecond = 0;
};
enum class HandoffState { Prepared, Committed, Aborted, Expired };
struct BrowserHandoff {
    QString origin, requestId, taskId, source, destination;
    HandoffState state = HandoffState::Prepared;
    qint64 created = 0, expires = 0;
};

// One connection, owned by the manager thread. All operations throw on failure.
class TaskStore {
public:
    explicit TaskStore(const QString& databasePath);
    ~TaskStore();
    TaskStore(const TaskStore&) = delete;
    TaskStore& operator=(const TaskStore&) = delete;
    QVector<Task> load();
    void save(const Task& task);
    void erase(const QString& id);
    BrowserHandoff prepareHandoff(const QString& origin, const QString& requestId,
        const DownloadRequest& request, const QString& destination, qint64 now, qint64 leaseMs = 30000);
    std::optional<BrowserHandoff> queryHandoff(const QString& origin, const QString& requestId, qint64 now);
    BrowserHandoff commitHandoff(const QString& origin, const QString& requestId, qint64 now);
    std::optional<BrowserHandoff> abortHandoff(const QString& origin, const QString& requestId, qint64 now);
    bool reserved(const DownloadRequest& request, const QString& destination, qint64 now);
private:
    sqlite3* db_ = nullptr;
};
QString destinationKey(const QString& path);
QString taskIdentity(const DownloadRequest& request);
}

#pragma once

#include "bt_engine.h"
#include "ed2k_task.h"
#include "http_engine.h"
#include "task.h"
#include "task_store.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

namespace litefetch {

// Command/snapshot boundary between a UI or CLI and the download engine.
// Commands are synchronous: Start blocks until the engine returns and the final
// state is persisted. Cancel and Pause may be called from another thread while
// Start is running. The service never reports a transition as successful unless
// its database commit succeeded.
class TaskService {
public:
    // Optional observer for live progress (called on the transferring thread).
    // It must be cheap and must not throw; failures are ignored so an observer
    // can never break the transfer or its persistence.
    using ProgressObserver = std::function<void(std::string_view taskId, const TransferProgressInfo&)>;
    // Torrent transfers go through their own engine so the core never links
    // libtorrent; the caller supplies it when BitTorrent is available.
    using BtTransfer = std::function<BtDownloadResult(const BtDownloadRequest&, const std::atomic_bool&,
        const std::function<void(const BtProgressInfo&)>&)>;

    // ed2k transfers go through their own engine for the same reason as BT.
    using Ed2kTransfer = std::function<Ed2kTaskResult(const Ed2kTaskRequest&,
        const std::atomic_bool&, const std::function<void(std::int64_t, std::int64_t)>&)>;

    // Free space on the volume that holds a path, or -1 when it cannot be read.
    using FreeSpaceFn = std::function<std::int64_t(const std::filesystem::path&)>;

    explicit TaskService(TaskRepository& repository, TransferFunction transfer = transferHttp,
        ProgressObserver observer = {}, BtTransfer btTransfer = {},
        Ed2kTransfer ed2kTransfer = {}, FreeSpaceFn freeSpace = {});

    // Storage/read failures during a command are returned as ok=false. Query
    // methods are not commands and throw StorageError if the read fails.
    CommandResult execute(const TaskCommand& command);
    std::optional<TaskSnapshot> snapshot(std::string_view id);
    std::vector<TaskSnapshot> snapshots();

    // Startup reconciliation: a Running row can only mean the previous process
    // died mid-transfer. Such rows become Paused, so a restart never resumes
    // silently. Returns the number of rows changed.
    int pauseInterrupted();

private:
    struct ActiveTransfer {
        std::shared_ptr<std::atomic_bool> cancelled;
        std::atomic_bool paused{false};
    };

    CommandResult add(const AddTaskCommand& command);
    CommandResult start(const StartTaskCommand& command);
    CommandResult pause(const PauseTaskCommand& command);
    CommandResult cancel(const CancelTaskCommand& command);
    CommandResult remove(const DeleteTaskCommand& command);
    CommandResult removeWithFiles(const DeleteTaskFilesCommand& command);
    CommandResult verify(const VerifyTaskCommand& command);
    CommandResult updateTorrent(const UpdateTorrentOptionsCommand& command);
    void unregister(std::string_view id, const std::shared_ptr<std::atomic_bool>& flag);
    // Shared implementation for Cancel and Pause: both stop the transfer, the
    // difference is the state the owning Start call persists.
    CommandResult stop(const std::string& taskId, bool pauseTransfer);

    TaskRepository& repository_;
    TransferFunction transfer_;
    BtTransfer btTransfer_;
    Ed2kTransfer ed2kTransfer_;
    FreeSpaceFn freeSpace_;
    ProgressObserver observer_;
    std::mutex activeMutex_;
    std::map<std::string, std::shared_ptr<ActiveTransfer>, std::less<>> active_;
};

} // namespace litefetch

#include "task_service.h"

#include "task_identity.h"
#include "text_utf8.h"

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <utility>

namespace litefetch {
namespace {
std::int64_t nowMillis()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool torrentSource(std::string_view source)
{
    if (source.rfind("magnet:", 0) == 0) return true;
    return source.size() > 8
        && (source.ends_with(".torrent") || source.ends_with(".TORRENT"));
}

bool httpSource(std::string_view source)
{
    auto startsWith = [&](std::string_view prefix) {
        if (source.size() < prefix.size()) return false;
        for (std::size_t i = 0; i < prefix.size(); ++i) {
            const char left = source[i];
            const char right = prefix[i];
            const char lowered = (left >= 'A' && left <= 'Z') ? static_cast<char>(left - 'A' + 'a') : left;
            if (lowered != right) return false;
        }
        return true;
    };
    return startsWith("http://") || startsWith("https://") || startsWith("ftp://");
}

CommandResult failure(std::string message)
{
    CommandResult result;
    result.ok = false;
    result.error = std::move(message);
    return result;
}

CommandResult storageFailure(const StorageError& error)
{
    return failure("storage: " + std::string(error.what()));
}

} // namespace

TaskService::TaskService(TaskRepository& repository, TransferFunction transfer,
    ProgressObserver observer, BtTransfer btTransfer)
    : repository_(repository), transfer_(std::move(transfer)), btTransfer_(std::move(btTransfer)),
      observer_(std::move(observer))
{
    if (!transfer_) transfer_ = transferHttp;
}

CommandResult TaskService::execute(const TaskCommand& command)
{
    try {
        if (const auto* addCommand = std::get_if<AddTaskCommand>(&command)) return add(*addCommand);
        if (const auto* startCommand = std::get_if<StartTaskCommand>(&command)) return start(*startCommand);
        if (const auto* pauseCommand = std::get_if<PauseTaskCommand>(&command))
            return pause(*pauseCommand);
        if (const auto* deleteCommand = std::get_if<DeleteTaskCommand>(&command)) return remove(*deleteCommand);
        if (const auto* updateCommand = std::get_if<UpdateTorrentOptionsCommand>(&command))
            return updateTorrent(*updateCommand);
        return cancel(std::get<CancelTaskCommand>(command));
    } catch (const StorageError& error) {
        // This also covers read failures before a command reaches its write.
        return storageFailure(error);
    } catch (const std::exception& error) {
        return failure("internal: " + std::string(error.what()));
    }
}

std::optional<TaskSnapshot> TaskService::snapshot(std::string_view id)
{
    return repository_.find(id);
}

std::vector<TaskSnapshot> TaskService::snapshots()
{
    return repository_.all();
}

CommandResult TaskService::add(const AddTaskCommand& command)
{
    const bool torrent = torrentSource(command.source);
    if (!torrent && !httpSource(command.source))
        return failure("only HTTP(S), FTP and BitTorrent sources are supported by the core boundary");
    if (torrent && command.source.rfind("magnet:", 0) != 0
        && !std::filesystem::is_regular_file(pathFromUtf8(command.source)))
        return failure("torrent file does not exist");
    const std::filesystem::path destination = pathFromUtf8(command.destination);
    if (command.destination.empty() || !destination.is_absolute())
        return failure("destination must be an absolute path");

    const auto baseId = taskIdentity(command.identity.empty() ? command.source : command.identity);
    std::string id = baseId;
    if (const auto existing = repository_.find(baseId)) {
        if (!command.allowDuplicate) {
            if (existing->destination != command.destination)
                return failure("task identity already exists with another destination");
            CommandResult result;
            result.ok = true;
            result.task = *existing;
            result.existing = true;
            return result;
        }
        if (existing->destination == command.destination)
            return failure("a duplicate copy needs a different destination");
        // The user explicitly asked for a second copy. Derive a new stable
        // identity from the same source without touching the existing row; the
        // chosen id is persisted, so it stays stable across restarts.
        bool allocated = false;
        for (int copy = 2; copy < 10000; ++copy) {
            const auto candidate = taskIdentity(
                (command.identity.empty() ? command.source : command.identity)
                + "\n#duplicate:" + std::to_string(copy));
            if (!repository_.find(candidate)) {
                id = candidate;
                allocated = true;
                break;
            }
        }
        if (!allocated) return failure("cannot allocate a duplicate task identity");
    }
    // A torrent saves into a directory, which normally already exists; the
    // single-file "do not overwrite" rule applies to HTTP/FTP destinations only.
    std::error_code existsError;
    if (!torrent && std::filesystem::exists(destination, existsError))
        return failure("destination already exists");
    if (existsError) return failure("cannot inspect destination: " + existsError.message());

    TaskSnapshot task;
    task.id = id;
    task.source = command.source;
    task.destination = command.destination;
    task.state = TaskState::Queued;
    task.total = -1;
    task.files = command.files;
    task.filePriorities = command.filePriorities;
    task.trackers = command.trackers;
    task.torrentFiles = command.torrentFiles;
    task.torrentPrivate = command.torrentPrivate;
    task.createdAt = nowMillis();
    task.updatedAt = task.createdAt;
    try {
        repository_.insert(task);
    } catch (const StorageError& error) {
        return storageFailure(error);
    }
    CommandResult result;
    result.ok = true;
    result.task = std::move(task);
    return result;
}

CommandResult TaskService::start(const StartTaskCommand& command)
{
    TaskSnapshot task;
    std::shared_ptr<ActiveTransfer> active;
    {
        // Keep the queued/running transition and active-slot registration in one
        // critical section so a queued Cancel cannot return success and then be
        // overwritten by a concurrent Start.
        std::lock_guard lock(activeMutex_);
        const auto loaded = repository_.find(command.taskId);
        if (!loaded) return failure("unknown task");
        task = *loaded;
        if (task.state == TaskState::Running) return failure("task is already running");
        if (task.state == TaskState::Completed) return failure("task is already completed");
        if (task.state == TaskState::Cancelled)
            return failure("task was cancelled; use an explicit retry command");
        if (active_.contains(command.taskId)) return failure("task is already running");
        active = std::make_shared<ActiveTransfer>();
        active->cancelled = std::make_shared<std::atomic_bool>(false);
        active_.emplace(command.taskId, active);

        // Progress survives a pause: the engine resumes from the partial file,
        // so the row keeps the bytes it already has instead of dropping to zero.
        task.state = TaskState::Running;
        task.error.clear();
        task.restartRequired = false;
        task.updatedAt = nowMillis();
        try {
            repository_.update(task);
        } catch (const StorageError& error) {
            // No transfer is started when the running transition cannot be committed.
            active_.erase(command.taskId);
            return storageFailure(error);
        } catch (...) {
            active_.erase(command.taskId);
            throw;
        }
    }

    struct Unregister {
        TaskService& service;
        std::string id;
        std::shared_ptr<std::atomic_bool> flag;
        bool armed = true;
        ~Unregister() { if (armed) service.unregister(id, flag); }
    } unregisterOnExit{*this, command.taskId, active->cancelled};

    std::string progressStorageError;
    std::int64_t lastPersist = nowMillis();
    auto progress = [&](const TransferProgressInfo& update) {
        task.received = update.received;
        task.total = update.total;
        if (observer_) {
            try {
                observer_(command.taskId, update);
            } catch (...) {
                // A UI observer must never stop or corrupt the transfer.
            }
        }
        const auto now = nowMillis();
        if (now - lastPersist < 250 && !(update.total >= 0 && update.received >= update.total)) return;
        lastPersist = now;
        task.updatedAt = now;
        try {
            repository_.update(task);
        } catch (const StorageError& error) {
            progressStorageError = error.what();
            active->cancelled->store(true);
        }
    };

    TransferResult transferResult;
    const bool torrent = torrentSource(task.source);
    if (torrent) {
        if (!btTransfer_) {
            transferResult.status = TransferStatus::Failed;
            transferResult.error = TransferError::Internal;
            transferResult.message = "the BitTorrent engine is not available in this build";
        } else {
            BtDownloadRequest request;
            request.source = task.source;
            request.savePath = std::filesystem::path(pathFromUtf8(task.destination)).string();
            for (std::size_t position = 0; position < task.files.size(); ++position) {
                const int priority = position < task.filePriorities.size()
                    ? task.filePriorities[position] : 1;
                request.choices.push_back(BtFileChoice{task.files[position], priority});
            }
            request.trackers = task.trackers;
            // Fast resume: the engine keeps its piece map next to the data so a
            // restart continues instead of rechecking every piece.
            request.resumePath = (std::filesystem::path(task.destination)
                / (".litefetch-" + task.id + ".resume")).string();
            try {
                const auto result = btTransfer_(request, *active->cancelled,
                    [&](const BtProgressInfo& update) {
                        progress(TransferProgressInfo{update.received, update.total, update.peers});
                    });
                transferResult.received = result.received;
                transferResult.total = -1;
                if (result.ok) {
                    transferResult.status = TransferStatus::Completed;
                } else if (result.cancelled) {
                    transferResult.status = TransferStatus::Cancelled;
                } else {
                    transferResult.status = TransferStatus::Failed;
                    transferResult.error = TransferError::Network;
                    transferResult.message = result.error;
                }
            } catch (const std::exception& error) {
                transferResult.status = TransferStatus::Failed;
                transferResult.error = TransferError::Internal;
                transferResult.message = error.what();
            }
        }
    } else {
        try {
            transferResult = transfer_(TransferRequest{task.source, task.destination,
                command.restartFromZero, command.username, command.password}, *active->cancelled, progress);
        } catch (const std::exception& error) {
            transferResult.status = TransferStatus::Failed;
            transferResult.error = TransferError::Internal;
            transferResult.message = error.what();
        } catch (...) {
            transferResult.status = TransferStatus::Failed;
            transferResult.error = TransferError::Internal;
            transferResult.message = "transfer raised an unknown exception";
        }
    }
    task.received = transferResult.received;
    task.total = transferResult.total;
    task.updatedAt = nowMillis();
    if (!progressStorageError.empty()) {
        task.state = TaskState::Failed;
        task.error = "storage: " + progressStorageError;
    } else if (transferResult.status == TransferStatus::Completed) {
        task.state = TaskState::Completed;
        task.error.clear();
        task.restartRequired = false;
    } else if (transferResult.restartRequired) {
        // The bytes on disk no longer match the resource. Nothing is deleted
        // until the user confirms a restart from zero.
        task.state = TaskState::Paused;
        task.restartRequired = true;
        task.error = transferResult.message.empty()
            ? "resource changed; confirm to restart" : transferResult.message;
    } else if (transferResult.status == TransferStatus::Cancelled) {
        task.state = active->paused.load() ? TaskState::Paused : TaskState::Cancelled;
        task.error.clear();
    } else {
        task.state = TaskState::Failed;
        task.error = transferResult.message.empty() ? "transfer failed" : transferResult.message;
    }

    try {
        repository_.update(task);
    } catch (const StorageError& error) {
        // The outcome (including a published file) is not reported as committed.
        CommandResult result = storageFailure(error);
        result.task = task;
        return result;
    }
    // The final row is committed; release the running slot before returning.
    unregister(command.taskId, active->cancelled);
    unregisterOnExit.armed = false;
    if (!progressStorageError.empty()) {
        // The final row was written, but the command still reports the storage
        // failure that stopped the transfer instead of claiming clean success.
        CommandResult result = failure("storage: " + progressStorageError);
        result.task = task;
        return result;
    }

    CommandResult result;
    result.ok = true;
    result.task = task;
    return result;
}

CommandResult TaskService::stop(const std::string& taskId, bool pauseTransfer)
{
    const auto target = pauseTransfer ? TaskState::Paused : TaskState::Cancelled;
    std::shared_ptr<ActiveTransfer> running;
    {
        std::lock_guard lock(activeMutex_);
        const auto active = active_.find(taskId);
        if (active != active_.end()) {
            running = active->second;
            running->paused.store(pauseTransfer);
            running->cancelled->store(true);
        } else {
            // The same lock covers Start's find -> register -> running commit, so
            // this queued stop either wins before Start or observes its active
            // slot; it cannot be overwritten afterwards.
            auto task = repository_.find(taskId);
            if (!task) return failure("unknown task");
            if (task->state == target) {
                CommandResult result;
                result.ok = true;
                result.task = *task;
                return result;
            }
            const bool stoppable = task->state == TaskState::Queued
                || task->state == TaskState::Running
                || (pauseTransfer && task->state == TaskState::Failed);
            if (!stoppable)
                return failure(pauseTransfer ? "task is not pausable" : "task is not cancellable");
            task->state = target;
            task->updatedAt = nowMillis();
            try {
                repository_.update(*task);
            } catch (const StorageError& error) {
                return storageFailure(error);
            }
            CommandResult result;
            result.ok = true;
            result.task = *task;
            return result;
        }
    }

    // Wait for the owning Start call to persist the actual outcome. A stop
    // request is not reported as success before that commit.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard lock(activeMutex_);
            if (!active_.contains(taskId)) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto task = repository_.find(taskId);
    if (!task) return failure("unknown task");
    if (task->state == target) {
        CommandResult result;
        result.ok = true;
        result.task = *task;
        return result;
    }
    CommandResult result = failure(pauseTransfer
        ? "pause was not committed (state " + std::string(taskStateName(task->state)) + ")"
        : "cancellation was not committed");
    result.task = *task;
    return result;
}

CommandResult TaskService::pause(const PauseTaskCommand& command)
{
    return stop(command.taskId, true);
}

CommandResult TaskService::cancel(const CancelTaskCommand& command)
{
    return stop(command.taskId, false);
}

int TaskService::pauseInterrupted()
{
    std::lock_guard lock(activeMutex_);
    int changed = 0;
    for (const auto& snapshot : repository_.all()) {
        if (snapshot.state != TaskState::Running) continue;
        if (active_.contains(snapshot.id)) continue; // live transfer in this process
        auto task = snapshot;
        task.state = TaskState::Paused;
        task.updatedAt = nowMillis();
        repository_.update(task);
        ++changed;
    }
    return changed;
}

CommandResult TaskService::updateTorrent(const UpdateTorrentOptionsCommand& command)
{
    std::lock_guard lock(activeMutex_);
    if (active_.contains(command.taskId))
        return failure("task is running; stop it before changing torrent options");
    const auto task = repository_.find(command.taskId);
    if (!task) return failure("unknown task");
    if (task->state == TaskState::Completed)
        return failure("a completed task keeps its selection");
    auto updated = *task;
    updated.files = command.files;
    updated.filePriorities = command.filePriorities;
    updated.trackers = command.trackers;
    updated.updatedAt = nowMillis();
    try {
        repository_.update(updated);
    } catch (const StorageError& error) {
        return storageFailure(error);
    }
    CommandResult result;
    result.ok = true;
    result.task = updated;
    return result;
}

CommandResult TaskService::remove(const DeleteTaskCommand& command)
{
    std::lock_guard lock(activeMutex_);
    if (active_.contains(command.taskId))
        return failure("task is running; stop it before deleting the record");
    const auto task = repository_.find(command.taskId);
    if (!task) return failure("unknown task");
    try {
        repository_.remove(command.taskId);
    } catch (const StorageError& error) {
        return storageFailure(error);
    }
    CommandResult result;
    result.ok = true;
    result.task = *task;
    return result;
}

void TaskService::unregister(std::string_view id, const std::shared_ptr<std::atomic_bool>& flag)
{
    std::lock_guard lock(activeMutex_);
    const auto found = active_.find(id);
    if (found != active_.end() && found->second->cancelled == flag) active_.erase(found);
}

} // namespace litefetch

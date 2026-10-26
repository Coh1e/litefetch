#include "task_service.h"

#include "task_identity.h"

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
    return startsWith("http://") || startsWith("https://");
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

TaskService::TaskService(TaskRepository& repository, TransferFunction transfer)
    : repository_(repository), transfer_(std::move(transfer))
{
    if (!transfer_) transfer_ = transferHttp;
}

CommandResult TaskService::execute(const TaskCommand& command)
{
    if (const auto* addCommand = std::get_if<AddTaskCommand>(&command)) return add(*addCommand);
    if (const auto* startCommand = std::get_if<StartTaskCommand>(&command)) return start(*startCommand);
    return cancel(std::get<CancelTaskCommand>(command));
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
    if (!httpSource(command.source)) return failure("only HTTP(S) sources are supported by the core boundary");
    const std::filesystem::path destination(command.destination);
    if (command.destination.empty() || !destination.is_absolute())
        return failure("destination must be an absolute path");

    const auto id = taskIdentity(command.source);
    if (const auto existing = repository_.find(id)) {
        if (existing->destination != command.destination)
            return failure("task identity already exists with another destination");
        CommandResult result;
        result.ok = true;
        result.task = *existing;
        return result;
    }
    std::error_code existsError;
    if (std::filesystem::exists(destination, existsError))
        return failure("destination already exists");
    if (existsError) return failure("cannot inspect destination: " + existsError.message());

    TaskSnapshot task;
    task.id = id;
    task.source = command.source;
    task.destination = command.destination;
    task.state = TaskState::Queued;
    task.total = -1;
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
    auto task = repository_.find(command.taskId);
    if (!task) return failure("unknown task");
    if (task->state == TaskState::Running) return failure("task is already running");
    if (task->state == TaskState::Completed) return failure("task is already completed");

    std::shared_ptr<std::atomic_bool> cancellation;
    {
        std::lock_guard lock(activeMutex_);
        if (active_.contains(command.taskId)) return failure("task is already running");
        cancellation = std::make_shared<std::atomic_bool>(false);
        active_.emplace(command.taskId, cancellation);
    }

    struct Unregister {
        TaskService& service;
        std::string id;
        std::shared_ptr<std::atomic_bool> flag;
        bool armed = true;
        ~Unregister() { if (armed) service.unregister(id, flag); }
    } unregisterOnExit{*this, command.taskId, cancellation};

    task->state = TaskState::Running;
    task->received = 0;
    task->total = -1;
    task->error.clear();
    task->updatedAt = nowMillis();
    try {
        repository_.update(*task);
    } catch (const StorageError& error) {
        // No transfer is started when the running transition cannot be committed.
        return storageFailure(error);
    }

    std::string progressStorageError;
    std::int64_t lastPersist = nowMillis();
    auto progress = [&](const TransferProgressInfo& update) {
        task->received = update.received;
        task->total = update.total;
        const auto now = nowMillis();
        if (now - lastPersist < 250 && !(update.total >= 0 && update.received >= update.total)) return;
        lastPersist = now;
        task->updatedAt = now;
        try {
            repository_.update(*task);
        } catch (const StorageError& error) {
            progressStorageError = error.what();
            cancellation->store(true);
        }
    };

    TransferResult transferResult;
    try {
        transferResult = transfer_(TransferRequest{task->source, task->destination}, *cancellation, progress);
    } catch (const std::exception& error) {
        transferResult.status = TransferStatus::Failed;
        transferResult.error = TransferError::Internal;
        transferResult.message = error.what();
    } catch (...) {
        transferResult.status = TransferStatus::Failed;
        transferResult.error = TransferError::Internal;
        transferResult.message = "transfer raised an unknown exception";
    }

    task->received = transferResult.received;
    task->total = transferResult.total;
    task->updatedAt = nowMillis();
    if (!progressStorageError.empty()) {
        task->state = TaskState::Failed;
        task->error = "storage: " + progressStorageError;
    } else if (transferResult.status == TransferStatus::Completed) {
        task->state = TaskState::Completed;
        task->error.clear();
    } else if (transferResult.status == TransferStatus::Cancelled) {
        task->state = TaskState::Cancelled;
        task->error.clear();
    } else {
        task->state = TaskState::Failed;
        task->error = transferResult.message.empty() ? "transfer failed" : transferResult.message;
    }

    try {
        repository_.update(*task);
    } catch (const StorageError& error) {
        // The outcome (including a published file) is not reported as committed.
        CommandResult result = storageFailure(error);
        result.task = *task;
        return result;
    }
    // The final row is committed; release the running slot before returning.
    unregister(command.taskId, cancellation);
    unregisterOnExit.armed = false;
    if (!progressStorageError.empty()) {
        // The final row was written, but the command still reports the storage
        // failure that stopped the transfer instead of claiming clean success.
        CommandResult result = failure("storage: " + progressStorageError);
        result.task = *task;
        return result;
    }

    CommandResult result;
    result.ok = true;
    result.task = *task;
    return result;
}

CommandResult TaskService::cancel(const CancelTaskCommand& command)
{
    std::shared_ptr<std::atomic_bool> running;
    {
        std::lock_guard lock(activeMutex_);
        const auto active = active_.find(command.taskId);
        if (active != active_.end()) running = active->second;
    }
    if (running) {
        running->store(true);
        // Wait for the owning Start call to persist the actual outcome. A
        // cancellation request is not reported as success before that commit.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard lock(activeMutex_);
                if (!active_.contains(command.taskId)) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto task = repository_.find(command.taskId);
        if (!task) return failure("unknown task");
        if (task->state == TaskState::Cancelled) {
            CommandResult result;
            result.ok = true;
            result.task = *task;
            return result;
        }
        CommandResult result = failure("cancellation was not committed");
        result.task = *task;
        return result;
    }
    auto task = repository_.find(command.taskId);
    if (!task) return failure("unknown task");
    if (task->state != TaskState::Queued && task->state != TaskState::Running)
        return failure("task is not cancellable");
    task->state = TaskState::Cancelled;
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

void TaskService::unregister(std::string_view id, const std::shared_ptr<std::atomic_bool>& flag)
{
    std::lock_guard lock(activeMutex_);
    const auto found = active_.find(id);
    if (found != active_.end() && found->second == flag) active_.erase(found);
}

} // namespace litefetch

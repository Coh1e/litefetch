#include "task_worker.h"

#include "http_engine.h"
#include "task_service.h"
#include "task_store.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <exception>
#include <utility>

namespace litefetch {

TaskWorker::TaskWorker(std::filesystem::path databasePath, void* notifyWindow)
    : databasePath_(std::move(databasePath)), notifyWindow_(notifyWindow)
{
}

TaskWorker::~TaskWorker()
{
    shutdown();
}

bool TaskWorker::start(std::string& error)
{
    if (started_) {
        error = "worker already started";
        return false;
    }
    try {
        repository_ = std::make_unique<TaskRepository>(databasePath_);
        service_ = std::make_unique<TaskService>(*repository_, transferHttp,
            [this](std::string_view taskId, const TransferProgressInfo& progress) {
                updateProgress(std::string(taskId), progress.received, progress.total);
            });
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
    started_ = true;
    controlThread_ = std::thread([this] { controlLoop(); });
    return true;
}

void TaskWorker::postAdd(std::string source, std::string destination, bool autoStart)
{
    Command command;
    command.type = CommandType::Add;
    command.source = std::move(source);
    command.destination = std::move(destination);
    command.autoStart = autoStart;
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        queue_.push_back(std::move(command));
    }
    condition_.notify_all();
}

void TaskWorker::postStart(std::string taskId)
{
    Command command;
    command.type = CommandType::Start;
    command.taskId = std::move(taskId);
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        queue_.push_back(std::move(command));
    }
    condition_.notify_all();
}

void TaskWorker::postCancel(std::string taskId)
{
    Command command;
    command.type = CommandType::Cancel;
    command.taskId = std::move(taskId);
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        queue_.push_back(std::move(command));
    }
    condition_.notify_all();
}

void TaskWorker::postRefresh()
{
    Command command;
    command.type = CommandType::Refresh;
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        queue_.push_back(std::move(command));
    }
    condition_.notify_all();
}

void TaskWorker::beginShutdown()
{
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
    }
    condition_.notify_all();
    wakeUi();
}

bool TaskWorker::shutdownComplete() const
{
    return shutdownFinished_.load();
}

bool TaskWorker::hasActiveTasks()
{
    std::lock_guard lock(mutex_);
    return !active_.empty();
}

void TaskWorker::join()
{
    if (joined_) return;
    joined_ = true;
    if (controlThread_.joinable()) controlThread_.join();
}

void TaskWorker::shutdown()
{
    beginShutdown();
    join();
}

WorkerSnapshot TaskWorker::snapshot()
{
    std::lock_guard lock(mutex_);
    WorkerSnapshot result;
    result.tasks = tasks_;
    result.active = active_;
    result.lastError = lastError_;
    result.lastErrorSequence = lastErrorSequence_;
    result.shuttingDown = stopping_;
    result.stopped = shutdownFinished_.load();
    return result;
}

void TaskWorker::controlLoop()
{
    SetThreadDescription(GetCurrentThread(), L"litefetch-task-worker");
    for (;;) {
        Command command;
        bool hasCommand = false;
        bool hasJobs = false;
        {
            std::unique_lock lock(mutex_);
            hasJobs = !jobs_.empty();
            const auto ready = [this] {
                return stopping_ || !queue_.empty() || anyJobFinished();
            };
            if (hasJobs) {
                // While a blocking Start runs, refresh the persisted snapshot
                // periodically so the row shows the committed running state
                // even before the first progress event arrives.
                condition_.wait_for(lock, std::chrono::milliseconds(250), ready);
            } else {
                condition_.wait(lock, ready);
            }
            if (!queue_.empty()) {
                command = std::move(queue_.front());
                queue_.pop_front();
                hasCommand = true;
            } else if (stopping_) {
                break;
            } else if (!hasJobs) {
                continue;
            }
        }
        if (hasCommand) execute(command);
        reapFinished();
        reload();
        wakeUi();
    }

    // Closing the window cancels queued and running work, then waits for the
    // blocking Start calls to persist their real outcome before returning.
    cancelAll();
    for (auto& job : jobs_) {
        if (job.thread.joinable()) job.thread.join();
    }
    jobs_.clear();
    reload();
    wakeUi();
    shutdownFinished_.store(true);
    condition_.notify_all();
}

void TaskWorker::execute(const Command& command)
{
    if (!service_) return;
    try {
        switch (command.type) {
        case CommandType::Add: {
            const auto result = service_->execute(AddTaskCommand{command.source, command.destination});
            if (!result.ok) {
                publishError("添加失败: " + (result.error.empty() ? "unknown error" : result.error));
                break;
            }
            if (command.autoStart) launchStart(result.task.id);
            break;
        }
        case CommandType::Start:
            launchStart(command.taskId);
            break;
        case CommandType::Cancel: {
            const auto result = service_->execute(CancelTaskCommand{command.taskId});
            if (!result.ok)
                publishError("停止失败: " + (result.error.empty() ? "unknown error" : result.error));
            break;
        }
        case CommandType::Refresh:
            break;
        }
    } catch (const std::exception& error) {
        publishError(std::string("命令失败: ") + error.what());
    } catch (...) {
        publishError("命令失败: unknown error");
    }
}

void TaskWorker::launchStart(const std::string& taskId)
{
    if (!service_) return;
    for (const auto& job : jobs_) {
        if (job.taskId == taskId && !job.finished->load()) return; // already running
    }
    auto finished = std::make_shared<std::atomic_bool>(false);
    StartJob job;
    job.taskId = taskId;
    job.finished = finished;
    try {
        job.thread = std::thread([this, taskId, finished] {
            SetThreadDescription(GetCurrentThread(), L"litefetch-transfer");
            try {
                const auto result = service_->execute(StartTaskCommand{taskId});
                if (!result.ok && !result.error.empty()) publishError("任务失败: " + result.error);
            } catch (const std::exception& error) {
                publishError(std::string("任务失败: ") + error.what());
            } catch (...) {
                publishError("任务失败: unknown error");
            }
            finished->store(true);
            condition_.notify_all();
            wakeUi();
        });
    } catch (const std::exception& error) {
        publishError(std::string("无法启动任务线程: ") + error.what());
        return;
    }
    jobs_.push_back(std::move(job));
    reload();
    wakeUi();
}

bool TaskWorker::anyJobFinished() const
{
    for (const auto& job : jobs_) {
        if (job.finished->load()) return true;
    }
    return false;
}

void TaskWorker::reapFinished()
{
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        if (!it->finished->load()) {
            ++it;
            continue;
        }
        if (it->thread.joinable()) it->thread.join();
        it = jobs_.erase(it);
    }
}

void TaskWorker::reload()
{
    if (!service_) return;
    std::vector<TaskSnapshot> tasks;
    try {
        tasks = service_->snapshots();
    } catch (const std::exception& error) {
        publishError(std::string("读取任务失败: ") + error.what());
        return;
    }
    std::vector<std::string> active;
    active.reserve(jobs_.size());
    for (const auto& job : jobs_) {
        if (!job.finished->load()) active.push_back(job.taskId);
    }
    {
        std::lock_guard lock(mutex_);
        tasks_ = std::move(tasks);
        active_ = std::move(active);
    }
}

void TaskWorker::cancelAll()
{
    if (!service_) return;
    std::vector<std::string> cancellable;
    try {
        for (const auto& task : service_->snapshots()) {
            if (task.state == TaskState::Queued || task.state == TaskState::Running)
                cancellable.push_back(task.id);
        }
    } catch (const std::exception& error) {
        publishError(std::string("读取任务失败: ") + error.what());
        return;
    }
    for (const auto& id : cancellable) {
        try {
            const auto result = service_->execute(CancelTaskCommand{id});
            if (!result.ok && !result.error.empty()) publishError("停止失败: " + result.error);
        } catch (const std::exception& error) {
            publishError(std::string("停止失败: ") + error.what());
        }
    }
}

void TaskWorker::wakeUi()
{
    if (notifyWindow_) PostMessageW(static_cast<HWND>(notifyWindow_), workerWakeMessage, 0, 0);
}

void TaskWorker::publishError(std::string message)
{
    {
        std::lock_guard lock(mutex_);
        lastError_ = std::move(message);
        ++lastErrorSequence_;
    }
    wakeUi();
}

void TaskWorker::updateProgress(const std::string& taskId, std::int64_t received, std::int64_t total)
{
    {
        std::lock_guard lock(mutex_);
        for (auto& task : tasks_) {
            if (task.id != taskId) continue;
            task.received = received;
            task.total = total;
            // TaskService commits `running` before the engine is called, so a
            // progress update proves the persisted state is at least running.
            // This closes the race where the cache was reloaded before that
            // commit and the row would otherwise stay "waiting" while bytes
            // are already moving.
            if (task.state == TaskState::Queued) task.state = TaskState::Running;
            break;
        }
    }
    wakeUi();
}

} // namespace litefetch

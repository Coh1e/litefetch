#include <algorithm>
#include "task_worker.h"

#include "low_usage.h"
#include "pipe_channel.h"
#include "schedule.h"

#include "http_engine.h"
#include "task_service.h"
#include "task_store.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <exception>
#include <fstream>
#include <set>
#include <utility>

namespace litefetch {

TaskWorker::TaskWorker(std::filesystem::path databasePath, void* notifyWindow,
    TaskService::BtTransfer btTransfer, TaskService::Ed2kTransfer ed2kTransfer)
    : databasePath_(std::move(databasePath)), notifyWindow_(notifyWindow),
      btTransfer_(std::move(btTransfer)), ed2kTransfer_(std::move(ed2kTransfer))
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
        // A Kad node makes ed2k tasks able to find sources without a server. The
        // contact file lives next to the database and is rewritten on exit.
        const auto contactsPath = (databasePath_.parent_path() / "kad-contacts.dat").string();
        try {
            kad_ = std::make_unique<KadNode>();
            std::string kadError;
            if (kad_->start(0, kadError)) kad_->loadContacts(contactsPath);
            else kad_.reset();
        } catch (const std::exception&) {
            kad_.reset();
        }
        TaskService::Ed2kTransfer ed2kWithKad = ed2kTransfer_;
        if (kad_) {
            ed2kWithKad = [this, contactsPath](const Ed2kTaskRequest& request,
                              const std::atomic_bool& cancelled,
                              const std::function<void(std::int64_t, std::int64_t)>& progress) {
                auto withLookup = request;
                // Settings win over the legacy server file when the user filled
                // the list in the options dialog.
                {
                    std::lock_guard lock(settingsMutex_);
                    if (!settings_.ed2kServers.empty()) {
                        withLookup.servers.clear();
                        for (const auto& entry : settings_.ed2kServers) {
                            std::string host;
                            std::uint16_t port = 0;
                            if (parseServerEndpoint(entry, host, port))
                                withLookup.servers.push_back({host, port});
                        }
                    }
                }
                withLookup.lookup = [this](const std::string& fileHash) {
                    std::vector<Ed2kSource> sources;
                    KadId id;
                    if (!KadId::fromHex(fileHash, id)) return sources;
                    for (const auto& contact : kad_->findSources(id, 500)) {
                        Ed2kSource source;
                        source.address = contact.address;
                        source.port = contact.port;
                        sources.push_back(source);
                    }
                    return sources;
                };
                withLookup.publish = [this](const std::string& fileHash,
                                               const std::string& host, std::uint16_t port) {
                    KadId id;
                    if (!KadId::fromHex(fileHash, id)) return;
                    KadContact self;
                    self.id = kad_->id();
                    self.address = host;
                    self.port = port;
                    kad_->publishSource(id, self, 500);
                };
                return ed2kTransfer_(withLookup, cancelled, progress);
            };
            (void)contactsPath;
        }
        // BitTorrent reports the bytes it sends while sharing; they belong in the
        // upload bucket, not in the download total of the file.
        auto btWithTraffic = [this](const BtDownloadRequest& request,
                                    const std::atomic_bool& cancelled,
                                    const std::function<void(const BtProgressInfo&)>& progress) {
            std::int64_t lastUploaded = 0;
            return btTransfer_(request, cancelled, [&](const BtProgressInfo& info) {
                if (service_ && info.uploaded > lastUploaded) {
                    service_->traffic().add(TrafficCategory::Upload, info.uploaded - lastUploaded);
                    lastUploaded = info.uploaded;
                }
                if (progress) progress(info);
            });
        };
        service_ = std::make_unique<TaskService>(*repository_, transferHttp,
            [this](std::string_view taskId, const TransferProgressInfo& progress) {
                updateProgress(std::string(taskId), progress.received, progress.total, progress.peers);
            },
            btWithTraffic, ed2kWithKad,
            [](const std::filesystem::path& path) -> std::int64_t {
                // Free bytes on the volume that holds the destination.
                ULARGE_INTEGER available{};
                if (!GetDiskFreeSpaceExW(path.c_str(), &available, nullptr, nullptr)) return -1;
                return static_cast<std::int64_t>(available.QuadPart);
            });
        // A running row can only come from a previous process that died; a
        // restart shows it as paused instead of resuming silently.
        service_->pauseInterrupted();
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
    started_ = true;
    controlThread_ = std::thread([this] { controlLoop(); });
    startBridge();
    return true;
}

void TaskWorker::postAdd(std::string source, std::string destination, bool autoStart,
    std::uint64_t batchId, int line, bool allowDuplicate, std::vector<int> files,
    std::string identity, std::vector<TorrentFileInfo> torrentFiles, bool torrentPrivate,
    std::string sha256)
{
    Command command;
    command.type = CommandType::Add;
    command.source = std::move(source);
    command.destination = std::move(destination);
    command.autoStart = autoStart;
    command.batchId = batchId;
    command.line = line;
    command.allowDuplicate = allowDuplicate;
    command.files = std::move(files);
    command.identity = std::move(identity);
    command.torrentFiles = std::move(torrentFiles);
    command.torrentPrivate = torrentPrivate;
    command.sha256 = std::move(sha256);
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        queue_.push_back(std::move(command));
    }
    condition_.notify_all();
}

void TaskWorker::postStart(std::string taskId, bool restartFromZero, std::string username,
    std::string password, bool forceVerify)
{
    Command command;
    command.type = CommandType::Start;
    command.taskId = std::move(taskId);
    command.restartFromZero = restartFromZero;
    command.username = std::move(username);
    command.password = std::move(password);
    command.forceVerify = forceVerify;
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        queue_.push_back(std::move(command));
    }
    condition_.notify_all();
}

void TaskWorker::postPause(std::string taskId)
{
    Command command;
    command.type = CommandType::Pause;
    command.taskId = std::move(taskId);
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        // A start that has not reached the service yet must not run at all.
        for (auto& job : jobs_) {
            if (job.taskId == command.taskId && job.cancelBeforeStart)
                job.cancelBeforeStart->store(true);
        }
        queue_.push_back(std::move(command));
    }
    condition_.notify_all();
}

void TaskWorker::postUpdateTorrent(std::string taskId, std::vector<int> files,
    std::vector<int> priorities, std::vector<std::string> trackers, int seedMode,
    int seedRatioMilli, int seedMinutes)
{
    Command command;
    command.type = CommandType::UpdateTorrent;
    command.taskId = std::move(taskId);
    command.files = std::move(files);
    command.priorities = std::move(priorities);
    command.trackers = std::move(trackers);
    command.seedMode = seedMode;
    command.seedRatioMilli = seedRatioMilli;
    command.seedMinutes = seedMinutes;
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
        for (auto& job : jobs_) {
            if (job.taskId == command.taskId && job.cancelBeforeStart)
                job.cancelBeforeStart->store(true);
        }
        queue_.push_back(std::move(command));
    }
    condition_.notify_all();
}

void TaskWorker::postDeleteFiles(std::string taskId)
{
    Command command;
    command.type = CommandType::DeleteFiles;
    command.taskId = std::move(taskId);
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        queue_.push_back(std::move(command));
    }
    condition_.notify_all();
}

void TaskWorker::postPriority(std::string taskId, int priority)
{
    Command command;
    command.type = CommandType::Priority;
    command.taskId = std::move(taskId);
    command.priority = priority;
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        queue_.push_back(std::move(command));
    }
    condition_.notify_all();
}

void TaskWorker::postVerify(std::string taskId)
{
    Command command;
    command.type = CommandType::Verify;
    command.taskId = std::move(taskId);
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        queue_.push_back(std::move(command));
    }
    condition_.notify_all();
}

void TaskWorker::postDelete(std::string taskId)
{
    Command command;
    command.type = CommandType::Delete;
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
    stopKad();
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
        // Commands that have not started must not write to the store after the
        // window decided to close. Active transfers are cancelled by cancelAll.
        queue_.clear();
    }
    condition_.notify_all();
    wakeUi();
}

void TaskWorker::rememberCredential(const std::string& host, const std::string& username,
    const std::string& password)
{
    CredentialStore store(databasePath_.parent_path() / "credentials.dat");
    store.save(host, username, password);
}

std::optional<StoredCredential> TaskWorker::storedCredential(const std::string& host) const
{
    CredentialStore store(databasePath_.parent_path() / "credentials.dat");
    return store.load(host);
}

void TaskWorker::forgetCredentials()
{
    // The file only holds DPAPI blobs; removing it forgets every host at once.
    std::error_code error;
    std::filesystem::remove(databasePath_.parent_path() / "credentials.dat", error);
}

void TaskWorker::startBridge()
{
    if (!service_) return;
    if (downloadDirectory_.empty()) downloadDirectory_ = databasePath_.parent_path() / "downloads";
    bridge_ = std::make_unique<browser::BrowserBridge>(*service_, downloadDirectory_);
    bridgeServer_ = std::make_unique<browser::BrowserPipeServer>(browser::defaultPipeName());
    std::string ignored;
    // A channel that cannot bind must not stop the downloader; the user simply
    // gets no browser handoff.
    bridgeServer_->start(*bridge_, ignored);
}

Settings TaskWorker::settings() const
{
    std::lock_guard lock(settingsMutex_);
    return settings_;
}

void TaskWorker::applySettings(const Settings& settings)
{
    {
        std::lock_guard lock(settingsMutex_);
        settings_ = settings;
    }
    if (repository_ && service_) {
        saveSettings(*repository_, settings);
        service_->setRateLimit(settings.speedLimitBytesPerSecond);
        service_->setHttpProxy(settings.httpProxy);
        service_->setRenameOnConflict(settings.renameOnConflict);
        service_->setSaveByCategory(settings.saveByCategory);
        {
            std::lock_guard lock(settingsMutex_);
            rateBands_ = settings.rateBands;
            schedule_ = settings.scheduledActions;
            baseLimit_ = settings.speedLimitBytesPerSecond;
            onAllComplete_ = settings.onAllComplete;
        }
        applyTimeBandLimit();
        setMaxConcurrent(settings.maxConcurrentDownloads);
    }
}

void TaskWorker::stopKad()
{
    if (bridgeServer_) {
        bridgeServer_->stop();
        bridgeServer_.reset();
    }
    bridge_.reset();
    if (!kad_) return;
    const auto contactsPath = (databasePath_.parent_path() / "kad-contacts.dat").string();
    kad_->saveContacts(contactsPath);
    kad_->stop();
    kad_.reset();
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
    for (auto& task : result.tasks) {
        const auto found = peers_.find(task.id);
        if (found != peers_.end()) task.peers = found->second;
    }
    result.active = active_;
    result.peers = peers_;
    result.addResults = addResults_;
    result.outcomes = outcomes_;
    result.lastError = lastError_;
    result.runtimeConcurrency = runtimeConcurrency_;
    result.allCompleteSequence = allCompleteSequence_;
    if (service_) {
        result.traffic[0] = service_->traffic().bytes(TrafficCategory::Download);
        result.traffic[1] = service_->traffic().bytes(TrafficCategory::Upload);
        result.traffic[2] = service_->traffic().bytes(TrafficCategory::Metadata);
        result.traffic[3] = service_->traffic().bytes(TrafficCategory::Verify);
    }
    result.lastErrorSequence = lastErrorSequence_;
    result.addResultSequence = addResultSequence_;
    result.outcomeSequence = outcomeSequence_;
    result.shuttingDown = stopping_;
    result.stopped = shutdownFinished_.load();
    return result;
}

void TaskWorker::controlLoop()
{
    SetThreadDescription(GetCurrentThread(), L"litefetch-task-worker");
    int bandTick = 0;
    // Existing rows must be visible on startup, before any command wakes the
    // control loop (the window refreshes its snapshot immediately after start).
    reload();
    wakeUi();
    for (;;) {
        Command command;
        bool hasCommand = false;
        bool hasJobs = false;
        {
            std::unique_lock lock(mutex_);
            hasJobs = !jobs_.empty();
            evaluateLowUsageLocked();
            bandTick += 1;
            // A band boundary must not wait for the next command.
            if (bandTick % 8 == 0) { applyTimeBandLimit(); runSchedule(); }
            const auto ready = [this] {
                return stopping_ || !queue_.empty() || anyJobFinished();
            };
            if (hasJobs) {
                // While a blocking Start runs, refresh the persisted snapshot
                // periodically so the row shows the committed running state
                // even before the first progress event arrives.
                condition_.wait_for(lock, std::chrono::milliseconds(250), ready);
            } else if (lowUsageIdleSeconds_ > 0) {
                // Without jobs the loop would sleep until a command arrives, but
                // low-usage mode has to notice that nothing is happening.
                condition_.wait_for(lock, std::chrono::milliseconds(500), ready);
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
        if (hasCommand) {
            noteActivity();
            execute(command);
        }
        reapFinished();
        reload();
        wakeUi();
    }

    // Closing the window cancels queued and running work, then waits for the
    // blocking Start calls to persist their real outcome before returning.
    try {
        cancelAll();
        for (auto& job : jobs_) {
            if (job.thread.joinable()) job.thread.join();
        }
        jobs_.clear();
        reload();
        wakeUi();
    } catch (const std::exception& error) {
        std::ofstream debug("D:/Projects/litefetch/tmp/shutdown-error.log", std::ios::app);
        debug << "controlLoop shutdown exception: " << error.what() << "\n";
    } catch (...) {
        std::ofstream debug("D:/Projects/litefetch/tmp/shutdown-error.log", std::ios::app);
        debug << "controlLoop shutdown unknown exception\n";
    }
    shutdownFinished_.store(true);
    condition_.notify_all();
}

void TaskWorker::execute(const Command& command)
{
    if (!service_) return;
    try {
        switch (command.type) {
        case CommandType::Add: {
            AddTaskCommand add;
            add.source = command.source;
            add.destination = command.destination;
            add.allowDuplicate = command.allowDuplicate;
            add.files = command.files;
            add.identity = command.identity;
            add.torrentFiles = command.torrentFiles;
            add.torrentPrivate = command.torrentPrivate;
            add.sha256 = command.sha256;
            const auto result = service_->execute(add);
            AddResult addResult;
            addResult.batchId = command.batchId;
            addResult.line = command.line;
            addResult.ok = result.ok;
            addResult.existing = result.existing;
            addResult.taskId = result.task.id;
            addResult.error = result.error;
            appendAddResult(addResult);
            if (!result.ok) {
                publishError("添加失败: " + (result.error.empty() ? "unknown error" : result.error));
                break;
            }
            if (command.autoStart) launchStart(result.task.id);
            break;
        }
        case CommandType::Start:
            launchStart(command.taskId, command.restartFromZero, command.username, command.password);
            break;
        case CommandType::Pause: {
            const auto result = service_->execute(PauseTaskCommand{command.taskId});
            if (!result.ok)
                publishError("暂停失败: " + (result.error.empty() ? "unknown error" : result.error));
            break;
        }
        case CommandType::Cancel: {
            const auto result = service_->execute(CancelTaskCommand{command.taskId});
            if (!result.ok)
                publishError("停止失败: " + (result.error.empty() ? "unknown error" : result.error));
            break;
        }
        case CommandType::UpdateTorrent: {
            UpdateTorrentOptionsCommand update;
            update.taskId = command.taskId;
            update.files = command.files;
            update.filePriorities = command.priorities;
            update.trackers = command.trackers;
            update.seedMode = command.seedMode;
            update.seedRatioMilli = command.seedRatioMilli;
            update.seedMinutes = command.seedMinutes;
            const auto result = service_->execute(update);
            if (!result.ok)
                publishError("属性应用失败: " + (result.error.empty() ? "unknown error" : result.error));
            break;
        }
        case CommandType::Priority: {
            const auto result = service_->execute(SetPriorityCommand{command.taskId, command.priority});
            if (!result.ok)
                publishError("设置优先级失败: " + (result.error.empty() ? "unknown error" : result.error));
            break;
        }
        case CommandType::Verify: {
            const auto result = service_->execute(VerifyTaskCommand{command.taskId});
            if (!result.ok)
                publishError("校验失败: " + (result.error.empty() ? "unknown error" : result.error));
            else if (result.task.verified)
                publishError("校验通过: " + result.task.id);
            else
                publishError("校验未通过: " + result.task.error);
            break;
        }
        case CommandType::DeleteFiles: {
            const auto result = service_->execute(DeleteTaskFilesCommand{command.taskId});
            if (!result.ok)
                publishError("删除任务及文件失败: " + (result.error.empty() ? "unknown error" : result.error));
            break;
        }
        case CommandType::Delete: {
            const auto before = service_->snapshot(command.taskId);
            if (!before) {
                appendOutcome(ActionOutcome{0, command.taskId, "delete", false, "unknown task"});
                publishError("删除失败: unknown task");
                break;
            }
            if (before->state == TaskState::Queued || before->state == TaskState::Running) {
                const auto cancelled = service_->execute(CancelTaskCommand{command.taskId});
                if (!cancelled.ok) {
                    const auto message = cancelled.error.empty() ? std::string("unknown error")
                                                                : cancelled.error;
                    appendOutcome(ActionOutcome{0, command.taskId, "delete", false,
                        "停止失败: " + message});
                    publishError("删除失败: 停止失败: " + message);
                    break;
                }
            }
            const auto result = service_->execute(DeleteTaskCommand{command.taskId});
            appendOutcome(ActionOutcome{0, command.taskId, "delete", result.ok, result.error});
            if (!result.ok)
                publishError("删除失败: " + (result.error.empty() ? "unknown error" : result.error));
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

void TaskWorker::applyTimeBandLimit()
{
    std::vector<RateBand> bands;
    std::int64_t base = 0;
    {
        std::lock_guard lock(settingsMutex_);
        bands = rateBands_;
        base = baseLimit_;
    }
    if (bands.empty()) {
        if (service_) service_->setRateLimit(base);
        return;
    }
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const int minute = static_cast<int>(now.wHour) * 60 + static_cast<int>(now.wMinute);
    if (service_) service_->setRateLimit(limitAtMinute(bands, minute, base));
}

void TaskWorker::runSchedule()
{
    std::vector<ScheduledAction> schedule;
    {
        std::lock_guard lock(settingsMutex_);
        schedule = schedule_;
    }
    if (schedule.empty()) return;
    int minute = 0;
    {
        std::lock_guard lock(settingsMutex_);
        if (clockMinute_) minute = clockMinute_();
        else {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            minute = static_cast<int>(now.wHour) * 60 + static_cast<int>(now.wMinute);
        }
    }
    const auto due = actionAtMinute(schedule, minute);
    if (!due) return;
    {
        std::lock_guard lock(settingsMutex_);
        if (!shouldFire(lastFiredMinute_, minute, *due)) return;
        lastFiredMinute_ = minute;
    }
    // The schedule acts on the whole list, not on a selection.
    for (const auto& task : service_->snapshots()) {
        if (due->start) {
            if (task.state == TaskState::Queued || task.state == TaskState::Paused
                || task.state == TaskState::Failed)
                launches_.push_back(task.id);
        } else if (task.state == TaskState::Running) {
            pauses_.push_back(task.id);
        }
    }
    // Commands must not run on the control thread, so queue them like any other.
    for (const auto& id : launches_) postStart(id);
    for (const auto& id : pauses_) postPause(id);
    launches_.clear();
    pauses_.clear();
}

void TaskWorker::setScheduleClockForTest(std::function<int()> minuteOfDay)
{
    std::lock_guard lock(settingsMutex_);
    clockMinute_ = std::move(minuteOfDay);
}

void TaskWorker::tickScheduleForTest()
{
    runSchedule();
}

void TaskWorker::noteActivity()
{
    std::lock_guard lock(mutex_);
    lastActivity_ = std::chrono::steady_clock::now();
}

void TaskWorker::evaluateLowUsageLocked()
{
    if (lowUsageIdleSeconds_ <= 0) {
        runtimeConcurrency_ = maxConcurrent_;
        return;
    }
    LowUsagePolicy policy;
    policy.enabled = true;
    policy.userConcurrency = maxConcurrent_;
    policy.idleAfter = std::chrono::seconds(lowUsageIdleSeconds_);
    const auto idle = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - lastActivity_);
    runtimeConcurrency_ = policy.effectiveConcurrency(static_cast<int>(jobs_.size()), idle);
}

void TaskWorker::setMaxConcurrent(int maxConcurrent)
{
    {
        std::lock_guard lock(mutex_);
        maxConcurrent_ = maxConcurrent < 0 ? 0 : maxConcurrent;
    }
    launchPendingStarts();
}

int TaskWorker::priorityOf(const std::string& taskId) const
{
    if (!service_) return 0;
    for (const auto& task : service_->snapshots()) {
        if (task.id == taskId) return task.priority;
    }
    return 0;
}

void TaskWorker::setLowUsageIdleSeconds(int seconds)
{
    std::lock_guard lock(mutex_);
    lowUsageIdleSeconds_ = seconds < 0 ? 0 : seconds;
    lastActivity_ = std::chrono::steady_clock::now();
}

void TaskWorker::launchPendingStarts()
{
    std::vector<PendingStart> ready;
    {
        std::lock_guard lock(mutex_);
        const int limit = runtimeConcurrency_ > 0 ? runtimeConcurrency_ : maxConcurrent_;
        if (limit <= 0) {
            ready.swap(pendingStarts_);
        } else {
            // Highest priority first, then oldest: the queue is stable so a
            // user does not have to guess which task jumps ahead.
            std::stable_sort(pendingStarts_.begin(), pendingStarts_.end(),
                [this](const PendingStart& left, const PendingStart& right) {
                    return priorityOf(left.taskId) > priorityOf(right.taskId);
                });
            // Count the ones already picked in this pass, so no phantom job is
            // added: a placeholder would never finish and would eat the slot.
            while (!pendingStarts_.empty()
                && static_cast<int>(jobs_.size() + ready.size()) < limit) {
                ready.push_back(pendingStarts_.front());
                pendingStarts_.erase(pendingStarts_.begin());
            }
        }
    }
    for (const auto& item : ready)
        launchStartNow(item.taskId, item.restartFromZero, item.username, item.password,
            item.forceVerify);
}

void TaskWorker::launchStart(const std::string& taskId, bool restartFromZero,
    std::string username, std::string password, bool forceVerify)
{
    if (!service_) return;
    {
        std::lock_guard lock(mutex_);
        for (const auto& job : jobs_) {
            if (job.taskId == taskId && !job.finished->load()) return;
        }
        for (const auto& pending : pendingStarts_) {
            if (pending.taskId == taskId) return;
        }
        const int limit = runtimeConcurrency_ > 0 ? runtimeConcurrency_ : maxConcurrent_;
        if (limit > 0 && static_cast<int>(jobs_.size()) >= limit) {
            PendingStart pending;
            pending.taskId = taskId;
            pending.restartFromZero = restartFromZero;
            pending.username = std::move(username);
            pending.password = std::move(password);
            pending.forceVerify = forceVerify;
            pendingStarts_.push_back(std::move(pending));
            return;
        }
    }
    launchStartNow(taskId, restartFromZero, std::move(username), std::move(password), forceVerify);
}

void TaskWorker::launchStartNow(const std::string& taskId, bool restartFromZero,
    std::string username, std::string password, bool forceVerify)
{
    if (!service_) return;
    for (const auto& job : jobs_) {
        if (job.taskId == taskId && !job.finished->load()) return; // already running
    }
    {
        std::lock_guard lock(mutex_);
        // A slot is full: remember the request instead of running it now.
        const int limit = runtimeConcurrency_ > 0 ? runtimeConcurrency_ : maxConcurrent_;
        if (limit > 0 && static_cast<int>(jobs_.size()) >= limit) {
            for (const auto& job : jobs_) {
                if (job.taskId == taskId && !job.finished->load()) return;
            }
            for (const auto& pending : pendingStarts_) {
                if (pending.taskId == taskId) return;
            }
            PendingStart pending;
            pending.taskId = taskId;
            pending.restartFromZero = restartFromZero;
            pending.username = username;
            pending.password = password;
            pending.forceVerify = forceVerify;
            pendingStarts_.push_back(std::move(pending));
            return;
        }
    }
    auto finished = std::make_shared<std::atomic_bool>(false);
    auto cancelFlag = std::make_shared<std::atomic_bool>(false);
    StartJob job;
    job.taskId = taskId;
    job.finished = finished;
    job.cancelBeforeStart = cancelFlag;
    try {
        job.thread = std::thread([this, taskId, restartFromZero, username, password, forceVerify,
                                      finished, cancelFlag] {
            SetThreadDescription(GetCurrentThread(), L"litefetch-transfer");
            try {
                StartTaskCommand start;
                start.taskId = taskId;
                start.restartFromZero = restartFromZero;
                start.username = username;
                start.password = password;
                start.forceVerify = forceVerify;
                start.cancelBeforeStart = cancelFlag;
                const auto result = service_->execute(start);
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
    {
        std::lock_guard lock(mutex_);
        // A batch is in flight; its end is what the completion action waits for.
        batchActive_ = true;
    }
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

void TaskWorker::noteBatchProgress()
{
    bool finished = false;
    {
        std::lock_guard lock(mutex_);
        if (!jobs_.empty() || !pendingStarts_.empty()) {
            batchActive_ = true;
            return;
        }
        if (!batchActive_) return;
        batchActive_ = false;
        finished = onAllComplete_ != 0;
        if (finished) ++allCompleteSequence_;
    }
    if (finished) wakeUi();
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
    launchPendingStarts();
    noteBatchProgress();
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
    // Only work with a live writer is cancelled. A queued record has no writer
    // and stays queued for the next explicit start; a persisted running row
    // without a local job is already an interrupted task and is left for the
    // recovery ticket rather than silently rewritten on close.
    std::set<std::string, std::less<>> activeJobs;
    for (const auto& job : jobs_) {
        if (!job.finished->load()) activeJobs.insert(job.taskId);
    }
    std::vector<std::string> cancellable;
    try {
        for (const auto& task : service_->snapshots()) {
            if (activeJobs.contains(task.id)) cancellable.push_back(task.id);
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

void TaskWorker::appendAddResult(const AddResult& result)
{
    {
        std::lock_guard lock(mutex_);
        AddResult stored = result;
        stored.sequence = ++addResultSequence_;
        addResults_.push_back(std::move(stored));
    }
    wakeUi();
}

void TaskWorker::appendOutcome(const ActionOutcome& outcome)
{
    {
        std::lock_guard lock(mutex_);
        ActionOutcome stored = outcome;
        stored.sequence = ++outcomeSequence_;
        outcomes_.push_back(std::move(stored));
    }
    wakeUi();
}

void TaskWorker::updateProgress(const std::string& taskId, std::int64_t received,
    std::int64_t total, int peers)
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
            peers_[taskId] = peers;
            break;
        }
    }
    wakeUi();
}

} // namespace litefetch

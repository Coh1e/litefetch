#pragma once

#include <array>
#include <functional>
#include <chrono>

#include "kad_node.h"
#include "credentials.h"
#include "schedule.h"
#include "settings.h"
#include "task.h"
#include "task_service.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace litefetch {

class TaskRepository;
class TaskService;

// Message posted to the window when worker state changed. WM_APP is 0x8000.
inline constexpr unsigned int workerWakeMessage = 0x8000u + 1;

struct AddResult {
    std::uint64_t sequence = 0;
    std::uint64_t batchId = 0;
    int line = 0; // 1-based source line, 0 for a single add
    bool ok = false;
    bool existing = false;
    std::string taskId;
    std::string error;
};

struct ActionOutcome {
    std::uint64_t sequence = 0;
    std::string taskId;
    std::string action;
    bool ok = false;
    std::string error;
};

struct WorkerSnapshot {
    std::vector<TaskSnapshot> tasks;
    std::vector<std::string> active;
    std::map<std::string, int, std::less<>> peers;
    std::vector<AddResult> addResults;
    std::vector<ActionOutcome> outcomes;
    std::string lastError;
    // The slot limit in force right now (low-usage mode lowers it when idle).
    int runtimeConcurrency = 0;
    // Traffic totals: download, upload, metadata, verify.
    std::array<std::int64_t, 4> traffic{};
    // Bumped once when the last running transfer ends and nothing is queued.
    std::uint64_t allCompleteSequence = 0;
    std::uint64_t lastErrorSequence = 0;
    std::uint64_t addResultSequence = 0;
    std::uint64_t outcomeSequence = 0;
    bool shuttingDown = false;
    bool stopped = false;
};

// Single command boundary between the Win32 window and the download core.
//
// The control thread owns the database/service for Add/Cancel/Refresh commands
// and reads snapshots; every Start runs on its own thread because the engine is
// blocking. That keeps the UI responsive and lets Cancel (called by the control
// thread) interrupt a running Start, which is the concurrency contract of
// TaskService. Closing the window cancels every queued/running task and joins
// all worker threads before the process exits.
class TaskWorker {
public:
    explicit TaskWorker(std::filesystem::path databasePath, void* notifyWindow = nullptr,
        TaskService::BtTransfer btTransfer = {}, TaskService::Ed2kTransfer ed2kTransfer = {});
    ~TaskWorker();
    TaskWorker(const TaskWorker&) = delete;
    TaskWorker& operator=(const TaskWorker&) = delete;

    bool start(std::string& error);
    void postAdd(std::string source, std::string destination, bool autoStart,
        std::uint64_t batchId = 0, int line = 0, bool allowDuplicate = false,
        std::vector<int> files = {}, std::string identity = {},
        std::vector<TorrentFileInfo> torrentFiles = {}, bool torrentPrivate = false,
        std::string sha256 = {});
    void postStart(std::string taskId, bool restartFromZero = false,
        std::string username = {}, std::string password = {}, bool forceVerify = false);
    void postPause(std::string taskId);
    void postUpdateTorrent(std::string taskId, std::vector<int> files, std::vector<int> priorities,
        std::vector<std::string> trackers, int seedMode = 0, int seedRatioMilli = 1000,
        int seedMinutes = 30);
    void postCancel(std::string taskId);
    void postDelete(std::string taskId);
    void postDeleteFiles(std::string taskId);
    void postVerify(std::string taskId);
    void postPriority(std::string taskId, int priority);
    // 0 means every task may run at once; a positive value keeps the extra
    // tasks queued until a transfer finishes.
    void setMaxConcurrent(int maxConcurrent);
    // Low-usage backoff: idle seconds (0 disables) below which nothing changes.
    void setLowUsageIdleSeconds(int seconds);
    void postRefresh();

    // Non-blocking shutdown request; the control thread cancels live tasks and
    // joins the start threads, then sets shutdownComplete().
    void beginShutdown();
    void stopKad();
    bool shutdownComplete() const;
    bool hasActiveTasks();

    // Joins the control thread. Safe to call after shutdownComplete(); also
    // callable directly for tests and the destructor.
    void join();
    void shutdown();

    WorkerSnapshot snapshot();
    // Test seams: replace the clock and tick the schedule without waiting for a
    // real minute to arrive.
    void setScheduleClockForTest(std::function<int()> minuteOfDay);
    void tickScheduleForTest();
    // Settings live in the task database; these keep the service and the ed2k
    // server list in step with what the options dialog stored.
    Settings settings() const;
    // Remembered credentials are protected with DPAPI for this Windows account.
    void rememberCredential(const std::string& host, const std::string& username,
        const std::string& password);
    std::optional<StoredCredential> storedCredential(const std::string& host) const;
    void forgetCredentials();
    void applySettings(const Settings& settings);

private:
    enum class CommandType { Add, Start, Pause, Cancel, Delete, DeleteFiles, Verify, Priority, Refresh, UpdateTorrent };
    struct Command {
        CommandType type = CommandType::Refresh;
        std::string source;
        std::string destination;
        std::string taskId;
        bool autoStart = false;
        bool allowDuplicate = false;
        bool restartFromZero = false;
        std::string username;
        std::string password;
        std::uint64_t batchId = 0;
        int line = 0;
        bool forceVerify = false;
        std::vector<int> files;
        std::string identity;
        std::vector<TorrentFileInfo> torrentFiles;
        std::vector<int> priorities;
        std::vector<std::string> trackers;
        bool torrentPrivate = false;
        std::string sha256;
        int priority = 0;
        int seedMode = 0;
        int seedRatioMilli = 1000;
        int seedMinutes = 30;
    };
    struct PendingStart {
        std::string taskId;
        bool restartFromZero = false;
        std::string username;
        std::string password;
        bool forceVerify = false;
    };

    struct StartJob {
        std::string taskId;
        std::thread thread;
        std::shared_ptr<std::atomic_bool> finished;
        // Set when a pause or stop arrives before the job reaches the service.
        std::shared_ptr<std::atomic_bool> cancelBeforeStart;
    };

    void controlLoop();
    void execute(const Command& command);
    // Recomputes the slot limit from the low-usage policy; call with the lock held.
    void evaluateLowUsageLocked();
    void runSchedule();
    void noteActivity();
    void noteBatchProgress();
    // Recomputes the limit from the time bands and hands it to the service.
    void applyTimeBandLimit();
    void launchPendingStarts();
    int priorityOf(const std::string& taskId) const;
    void launchStartNow(const std::string& taskId, bool restartFromZero, std::string username,
        std::string password, bool forceVerify);
    void launchStart(const std::string& taskId, bool restartFromZero = false,
        std::string username = {}, std::string password = {}, bool forceVerify = false);
    void reapFinished();
    void reload();
    void cancelAll();
    void wakeUi();
    void publishError(std::string message);
    void appendAddResult(const AddResult& result);
    void appendOutcome(const ActionOutcome& outcome);
    void updateProgress(const std::string& taskId, std::int64_t received, std::int64_t total,
        int peers = 0);
    bool anyJobFinished() const;

    std::filesystem::path databasePath_;
    void* notifyWindow_ = nullptr;
    std::unique_ptr<TaskRepository> repository_;
    std::unique_ptr<TaskService> service_;
    TaskService::BtTransfer btTransfer_;
    TaskService::Ed2kTransfer ed2kTransfer_;
    std::unique_ptr<KadNode> kad_;
    mutable std::mutex settingsMutex_;
    std::vector<RateBand> rateBands_;
    std::vector<ScheduledAction> schedule_;
    int lastFiredMinute_ = -1;
    std::function<int()> clockMinute_;
    std::vector<std::string> launches_;
    std::vector<std::string> pauses_;
    int onAllComplete_ = 0;
    std::uint64_t allCompleteSequence_ = 0;
    // True while work is in flight, so the completion fires once per batch.
    bool batchActive_ = false;
    std::int64_t baseLimit_ = 0;
    Settings settings_;
    std::thread controlThread_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Command> queue_;
    bool started_ = false;
    bool stopping_ = false;
    bool joined_ = false;
    std::atomic_bool shutdownFinished_{false};
    std::vector<StartJob> jobs_;
    std::vector<PendingStart> pendingStarts_;
    int maxConcurrent_ = 0;
    int lowUsageIdleSeconds_ = 0;
    int runtimeConcurrency_ = 0;
    std::chrono::steady_clock::time_point lastActivity_ = std::chrono::steady_clock::now(); // control thread only
    std::vector<TaskSnapshot> tasks_;
    std::vector<std::string> active_;
    std::map<std::string, int, std::less<>> peers_;
    std::vector<AddResult> addResults_;
    std::vector<ActionOutcome> outcomes_;
    std::string lastError_;
    std::uint64_t lastErrorSequence_ = 0;
    std::uint64_t addResultSequence_ = 0;
    std::uint64_t outcomeSequence_ = 0;
};

} // namespace litefetch

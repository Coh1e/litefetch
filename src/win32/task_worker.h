#pragma once

#include "task.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
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

struct WorkerSnapshot {
    std::vector<TaskSnapshot> tasks;
    std::vector<std::string> active;
    std::string lastError;
    std::uint64_t lastErrorSequence = 0;
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
    explicit TaskWorker(std::filesystem::path databasePath, void* notifyWindow = nullptr);
    ~TaskWorker();
    TaskWorker(const TaskWorker&) = delete;
    TaskWorker& operator=(const TaskWorker&) = delete;

    bool start(std::string& error);
    void postAdd(std::string source, std::string destination, bool autoStart);
    void postStart(std::string taskId);
    void postCancel(std::string taskId);
    void postRefresh();

    // Non-blocking shutdown request; the control thread cancels live tasks and
    // joins the start threads, then sets shutdownComplete().
    void beginShutdown();
    bool shutdownComplete() const;
    bool hasActiveTasks();

    // Joins the control thread. Safe to call after shutdownComplete(); also
    // callable directly for tests and the destructor.
    void join();
    void shutdown();

    WorkerSnapshot snapshot();

private:
    enum class CommandType { Add, Start, Cancel, Refresh };
    struct Command {
        CommandType type = CommandType::Refresh;
        std::string source;
        std::string destination;
        std::string taskId;
        bool autoStart = false;
    };
    struct StartJob {
        std::string taskId;
        std::thread thread;
        std::shared_ptr<std::atomic_bool> finished;
    };

    void controlLoop();
    void execute(const Command& command);
    void launchStart(const std::string& taskId);
    void reapFinished();
    void reload();
    void cancelAll();
    void wakeUi();
    void publishError(std::string message);
    void updateProgress(const std::string& taskId, std::int64_t received, std::int64_t total);
    bool anyJobFinished() const;

    std::filesystem::path databasePath_;
    void* notifyWindow_ = nullptr;
    std::unique_ptr<TaskRepository> repository_;
    std::unique_ptr<TaskService> service_;
    std::thread controlThread_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Command> queue_;
    bool started_ = false;
    bool stopping_ = false;
    bool joined_ = false;
    std::atomic_bool shutdownFinished_{false};
    std::vector<StartJob> jobs_; // control thread only
    std::vector<TaskSnapshot> tasks_;
    std::vector<std::string> active_;
    std::string lastError_;
    std::uint64_t lastErrorSequence_ = 0;
};

} // namespace litefetch

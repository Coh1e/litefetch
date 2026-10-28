#pragma once

#include "task.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace litefetch::ui {

// One row of the Details list. Text fields are UTF-8 and mirror exactly what
// the six design columns show; speed and remaining time come from measured
// progress deltas, never from a placeholder.
struct TaskRow {
    std::string id;
    std::string source;
    std::string destination;
    std::string name;
    std::string size;
    std::string progress;
    std::string speed;
    std::string remaining;
    std::string status;
    std::string error; // persisted task error for the native infotip
    // Optional-column values. Only real task/engine data is filled here;
    // upload/connection/source counts stay empty until an engine reports them.
    std::string protocol;
    std::string address;   // redacted source for display, copy and search
    std::string savePath;
    std::string uploadSpeed;
    std::string connections;
    std::string sources;
    std::string uploaded;
    TaskState state = TaskState::Queued;
    bool active = false;
    bool interrupted = false;
    // Stored bytes no longer match the resource; restarting asks first.
    bool restartRequired = false;
    // BitTorrent detail for the properties page.
    std::vector<TorrentFileInfo> torrentFiles;
    std::vector<int> filePriorities;
    std::vector<std::string> trackers;
    bool torrentPrivate = false;
    std::int64_t uploadedBytes = 0;
    int seedMode = 0;
    int seedRatioMilli = 1000;
    int seedMinutes = 30;
    std::int64_t received = 0;
    std::int64_t total = -1;
    double speedBytesPerSecond = 0.0;
    bool speedKnown = false;
    bool remainingKnown = false;
    double remainingSeconds = 0.0;
    std::int64_t createdAt = 0;
    std::int64_t completedAt = 0; // 0 means unknown (not completed)
    std::int64_t updatedAt = 0;
};

// Command availability is derived from the real capability of this build:
// pause/resume (#5), delete (#4) and options are not implemented and stay
// disabled instead of pretending to work.
struct ActionState {
    bool newDownload = true;
    bool start = false;
    bool pause = false;
    bool stop = false;
    bool deleteTask = false;
    bool openFile = false;
    bool openFolder = false;
    bool options = false;
    bool copyLink = false;
    bool copyPath = false;
    bool refresh = true;
    // Counts over the visible selected TaskIds. Commands act on the eligible
    // subset only and the UI reports these counts.
    std::size_t selectedCount = 0;
    std::size_t startCount = 0;
    std::size_t pauseCount = 0;
    std::size_t stopCount = 0;
    std::size_t deleteCount = 0;
    std::size_t openFileCount = 0;
    std::size_t openFolderCount = 0;
    std::size_t copyLinkCount = 0;
    std::size_t copyPathCount = 0;
};

struct StatusTexts {
    std::string tasks;
    std::string active;
    std::string download;
    std::string upload;
};

class TaskListModel {
public:
    // `activeIds` are tasks with a worker thread in this process. A persisted
    // Running row without an active worker is an interrupted task, not a live
    // download; crash recovery is a later ticket and must not be implied.
    void update(std::vector<TaskSnapshot> tasks, const std::vector<std::string>& activeIds,
        std::int64_t nowMillis);

    const std::vector<TaskRow>& rows() const { return rows_; }
    std::size_t indexOf(std::string_view id) const;
    ActionState actionsFor(std::string_view selectedId) const;
    ActionState actionsForSelection(const std::vector<std::string>& selectedIds) const;
    StatusTexts status() const;
    // First part shows 可见: visible / total when a filter/query is active.
    // Parts 2-4 stay whole-queue totals, never a sum of filtered rows.
    StatusTexts status(std::size_t visibleCount, bool filtered) const;
    double totalDownloadSpeed() const;

private:
    struct Sample {
        std::int64_t received = 0;
        std::int64_t at = 0;
        double speed = 0.0;
    };
    std::vector<TaskRow> rows_;
    std::map<std::string, Sample, std::less<>> samples_;
};

} // namespace litefetch::ui

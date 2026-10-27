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
    TaskState state = TaskState::Queued;
    bool active = false;
    bool interrupted = false;
    std::int64_t received = 0;
    std::int64_t total = -1;
    double speedBytesPerSecond = 0.0;
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
    bool openFolder = false;
    bool options = false;
    bool copyLink = false;
    bool refresh = true;
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
    StatusTexts status() const;
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

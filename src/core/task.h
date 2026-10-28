#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace litefetch {

// Persisted task states. The text form is the storage format and must stay
// append-only; never renumber or silently rename an existing state.
enum class TaskState { Queued, Running, Seeding, Completed, Failed, Cancelled, Paused };

std::string_view taskStateName(TaskState state);
std::optional<TaskState> parseTaskState(std::string_view name);

// One file inside a torrent, as reported by the metadata.
struct TorrentFileInfo {
    std::string path;
    std::int64_t size = 0;
};

struct TaskSnapshot {
    std::string id;
    std::string source;
    std::string destination;
    TaskState state = TaskState::Queued;
    std::int64_t received = 0;
    std::int64_t total = -1; // -1 until the server reports a length.
    std::string error;
    // The stored partial data no longer matches the resource: starting again
    // needs the user's confirmation before the bytes are discarded.
    bool restartRequired = false;
    // BitTorrent only: the file indices the user confirmed for this task and
    // the matching priorities (0 skip, 1 normal, 2 high). Both are parallel.
    std::vector<int> files;
    std::vector<int> filePriorities;
    // BitTorrent only: extra trackers the user added for this task.
    std::vector<std::string> trackers;
    // BitTorrent only: the file list and privacy flag from the metadata.
    std::vector<TorrentFileInfo> torrentFiles;
    bool torrentPrivate = false;
    // BitTorrent only: bytes uploaded to peers, summed across runs, and the
    // seed budget (mode 0 stop, 1 ratio in thousandths, 2 minutes).
    std::int64_t uploaded = 0;
    int seedMode = 0;
    int seedRatioMilli = 1000;
    int seedMinutes = 30;
    // Live peer count reported by the engine; never persisted.
    int peers = 0;
    std::int64_t createdAt = 0;
    std::int64_t updatedAt = 0;
};

struct AddTaskCommand {
    std::string source;
    std::string destination;
    // Default false: a source that is already known locates the existing task
    // instead of creating a second row. An explicit user choice to keep another
    // copy sets this and receives a new, restart-stable identity.
    bool allowDuplicate = false;
    // BitTorrent only: the confirmed file indices; empty means every file.
    std::vector<int> files;
    std::vector<int> filePriorities;
    std::vector<std::string> trackers;
    // BitTorrent only: the info hash learned from metadata. It identifies the
    // content itself, so a magnet and a .torrent for the same torrent share one
    // task even though their source strings differ.
    std::string identity;
    // BitTorrent only: the metadata the selection dialog showed, kept so the
    // properties page can list the files without re-fetching.
    std::vector<TorrentFileInfo> torrentFiles;
    bool torrentPrivate = false;
};
struct StartTaskCommand {
    std::string taskId;
    // The user confirmed starting over after the server refused to resume or
    // the resource changed. Otherwise the stored partial bytes are kept.
    bool restartFromZero = false;
    // Site credentials for this start only. They are not stored with the task;
    // the "remember" feature is a later ticket.
    std::string username;
    std::string password;
    // Recheck every piece instead of trusting what is on disk (the manual
    // "verify" action).
    bool forceVerify = false;
};
// Stops a running transfer but keeps the partial data and marks the task
// paused; Start resumes from the stored offset.
struct PauseTaskCommand {
    std::string taskId;
};
struct CancelTaskCommand {
    std::string taskId;
};
struct DeleteTaskCommand {
    std::string taskId;
};
// Removes the task and every file it created: the destination, the resume
// files and the ed2k part records. Anything that cannot be removed is
// reported per path and the record stays so the user can retry.
struct DeleteTaskFilesCommand {
    std::string taskId;
};
// Replaces the BitTorrent selection and trackers of an existing task. Only a
// paused, queued or failed task may be changed; a running transfer keeps its
// own choices until it stops.
struct UpdateTorrentOptionsCommand {
    std::string taskId;
    std::vector<int> files;
    std::vector<int> filePriorities;
    std::vector<std::string> trackers;
    std::vector<TorrentFileInfo> torrentFiles;
    bool torrentPrivate = false;
    int seedMode = 0;
    int seedRatioMilli = 1000;
    int seedMinutes = 30;
};

using TaskCommand = std::variant<AddTaskCommand, StartTaskCommand, PauseTaskCommand,
    CancelTaskCommand, DeleteTaskCommand, DeleteTaskFilesCommand, UpdateTorrentOptionsCommand>;

struct CommandResult {
    // True only when the requested transition was validated and committed (for
    // Start, the actual transfer outcome, including Failed, was persisted).
    // Storage failures are never reported as success.
    bool ok = false;
    TaskSnapshot task;
    std::string error;
    // Add only: the command located an existing row with the same source and
    // destination instead of inserting a new one.
    bool existing = false;
};

} // namespace litefetch

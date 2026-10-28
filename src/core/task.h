#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace litefetch {

// Persisted task states. The text form is the storage format and must stay
// append-only; never renumber or silently rename an existing state.
enum class TaskState { Queued, Running, Completed, Failed, Cancelled };

std::string_view taskStateName(TaskState state);
std::optional<TaskState> parseTaskState(std::string_view name);

struct TaskSnapshot {
    std::string id;
    std::string source;
    std::string destination;
    TaskState state = TaskState::Queued;
    std::int64_t received = 0;
    std::int64_t total = -1; // -1 until the server reports a length.
    std::string error;
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
};
struct StartTaskCommand {
    std::string taskId;
};
struct CancelTaskCommand {
    std::string taskId;
};
struct DeleteTaskCommand {
    std::string taskId;
};

using TaskCommand = std::variant<AddTaskCommand, StartTaskCommand, CancelTaskCommand, DeleteTaskCommand>;

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

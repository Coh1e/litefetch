#include "task.h"

namespace litefetch {

std::string_view taskStateName(TaskState state)
{
    switch (state) {
    case TaskState::Queued: return "queued";
    case TaskState::Running: return "running";
    case TaskState::Completed: return "completed";
    case TaskState::Failed: return "failed";
    case TaskState::Cancelled: return "cancelled";
    case TaskState::Paused: return "paused";
    }
    return "unknown";
}

std::optional<TaskState> parseTaskState(std::string_view name)
{
    if (name == "queued") return TaskState::Queued;
    if (name == "running") return TaskState::Running;
    if (name == "completed") return TaskState::Completed;
    if (name == "failed") return TaskState::Failed;
    if (name == "cancelled") return TaskState::Cancelled;
    if (name == "paused") return TaskState::Paused;
    return std::nullopt;
}

} // namespace litefetch

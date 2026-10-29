#include "notification_policy.h"

namespace litefetch::ui {

bool NotificationPolicy::shouldNotify(const std::string& taskId, TaskState state)
{
    if (state != TaskState::Completed && state != TaskState::Failed) return false;
    return notified_.insert(taskId).second;
}

void NotificationPolicy::forget(const std::string& taskId)
{
    notified_.erase(taskId);
}

} // namespace litefetch::ui

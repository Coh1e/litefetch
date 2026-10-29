#pragma once

#include "task.h"

#include <set>
#include <string>

namespace litefetch::ui {

// Decides when a finished task deserves a notification: only the first time it
// reaches a terminal state, so a refresh loop does not repeat itself, and never
// for states the user caused (paused, cancelled).
class NotificationPolicy {
public:
    bool shouldNotify(const std::string& taskId, TaskState state);
    void forget(const std::string& taskId);
    void clear() { notified_.clear(); }

private:
    std::set<std::string, std::less<>> notified_;
};

} // namespace litefetch::ui

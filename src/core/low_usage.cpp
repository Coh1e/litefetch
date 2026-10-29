#include "low_usage.h"

#include <algorithm>

namespace litefetch {

int LowUsagePolicy::effectiveConcurrency(int activeTransfers, std::chrono::seconds idle) const
{
    (void)activeTransfers;
    if (!enabled) return userConcurrency;
    if (idle < idleAfter) return userConcurrency;
    // Idle: at most one transfer, and never more than what the user allowed.
    if (userConcurrency > 0) return std::min(userConcurrency, 1);
    return 1;
}

bool LowUsagePolicy::shouldPauseSeeding(std::chrono::seconds idle, int queuedTasks) const
{
    if (!enabled) return false;
    // A queued task means the user still wants the link busy.
    if (queuedTasks > 0) return false;
    return idle >= idleAfter;
}

} // namespace litefetch

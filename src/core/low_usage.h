#pragma once

#include <chrono>
#include <cstdint>

namespace litefetch {

// Low-usage mode: when the app has been idle for a while it stops keeping the
// link busy. The policy is a pure function so the behaviour can be tested
// without waiting for real time to pass.
struct LowUsagePolicy {
    bool enabled = false;
    // The user's own limit; 0 means unlimited.
    int userConcurrency = 0;
    // How long without any transfer activity before the app backs off.
    std::chrono::seconds idleAfter{60};

    // Effective number of transfers allowed right now.
    int effectiveConcurrency(int activeTransfers, std::chrono::seconds idle) const;
    // Whether seeding should pause: idle in low-usage mode, and nothing queued.
    bool shouldPauseSeeding(std::chrono::seconds idle, int queuedTasks) const;
};

} // namespace litefetch
